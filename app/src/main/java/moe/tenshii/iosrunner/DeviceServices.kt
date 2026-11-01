package moe.tenshii.iosrunner

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import org.json.JSONObject

/** The Android services behind the frameworks a guest app calls: permissions, notifications, location and network state. */
class DeviceServices(private val activity: Activity, private val mainThread: Handler, private val reply: (Int, String) -> Unit) {
    private val preferences = activity.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
    private val pendingNotifications = HashMap<String, Runnable>()
    private val locationListeners = HashMap<Int, LocationListener>()
    private val networkCallbacks = HashMap<Int, ConnectivityManager.NetworkCallback>()
    private val connectivity = activity.getSystemService(ConnectivityManager::class.java)
    private val locations = activity.getSystemService(LocationManager::class.java)
    private val notifications = activity.getSystemService(NotificationManager::class.java)
    private var nextNotificationId = 1

    fun receive(kind: String, payload: JSONObject) {
        when (kind) {
            "permission" -> requestPermission(payload)
            "notify" -> scheduleNotification(payload)
            "notify-cancel" -> cancelNotification(payload.getString("identifier"))
            "location-start" -> startLocation(payload.getInt("id"))
            "location-stop" -> stopLocation(payload.getInt("id"))
            "network-start" -> startNetwork(payload.getInt("id"))
            "network-stop" -> stopNetwork(payload.getInt("id"))
        }
    }

    /** "granted", "denied", or "undetermined" before the user has ever been asked. */
    fun permissionStatus(group: String): String {
        val names = permissionsFor(group)
        return when {
            names.isEmpty() || names.any { activity.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED } -> "granted"
            preferences.getBoolean(group, false) -> "denied"
            else -> "undetermined"
        }
    }

    fun onPermissionResult(requestCode: Int, results: IntArray) {
        reply(requestCode, JSONObject().put("granted", results.any { it == PackageManager.PERMISSION_GRANTED }).toString())
    }

    private fun requestPermission(payload: JSONObject) {
        val group = payload.getString("group")
        val id = payload.getInt("id")
        val names = permissionsFor(group)
        if (names.isEmpty()) {
            reply(id, JSONObject().put("granted", true).toString())
            return
        }
        preferences.edit().putBoolean(group, true).apply()
        activity.requestPermissions(names.toTypedArray(), id)
    }

    private fun permissionsFor(group: String): List<String> = when (group) {
        GROUP_LOCATION -> listOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        GROUP_NOTIFICATIONS -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) listOf(Manifest.permission.POST_NOTIFICATIONS) else emptyList()
        else -> emptyList()
    }

    private fun scheduleNotification(payload: JSONObject) {
        val identifier = payload.getString("identifier")
        val title = payload.optString("title")
        val subtitle = payload.optString("subtitle")
        val body = payload.optString("body")
        cancelNotification(identifier)
        val post = Runnable {
            pendingNotifications.remove(identifier)
            postNotification(title, subtitle, body)
        }
        pendingNotifications[identifier] = post
        mainThread.postDelayed(post, (payload.optDouble("delay", 0.0) * MILLISECONDS_PER_SECOND).toLong())
    }

    private fun postNotification(title: String, subtitle: String, body: String) {
        if (permissionStatus(GROUP_NOTIFICATIONS) != "granted") return
        notifications.createNotificationChannel(NotificationChannel(CHANNEL, activity.getString(R.string.app_name), NotificationManager.IMPORTANCE_DEFAULT))
        val notification = Notification.Builder(activity, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(title)
            .setSubText(subtitle.ifEmpty { null })
            .setContentText(body)
            .setStyle(Notification.BigTextStyle().bigText(body))
            .setAutoCancel(true)
            .build()
        notifications.notify(nextNotificationId++, notification)
    }

    private fun cancelNotification(identifier: String) {
        pendingNotifications.remove(identifier)?.let { mainThread.removeCallbacks(it) }
    }

    @SuppressLint("MissingPermission")
    private fun startLocation(id: Int) {
        if (permissionStatus(GROUP_LOCATION) != "granted") {
            reply(id, JSONObject().put("error", "denied").toString())
            return
        }
        val providers = locations.getProviders(true).filter { it != LocationManager.PASSIVE_PROVIDER }
        if (providers.isEmpty()) {
            reply(id, JSONObject().put("error", "unavailable").toString())
            return
        }
        val listener = LocationListener { reply(id, describe(it)) }
        locationListeners[id] = listener
        providers.forEach { provider ->
            locations.getLastKnownLocation(provider)?.let { reply(id, describe(it)) }
            locations.requestLocationUpdates(provider, LOCATION_INTERVAL_MS, 0f, listener, mainThread.looper)
        }
    }

    private fun stopLocation(id: Int) {
        locationListeners.remove(id)?.let { locations.removeUpdates(it) }
    }

    private fun describe(location: Location): String = JSONObject()
        .put("latitude", location.latitude)
        .put("longitude", location.longitude)
        .put("altitude", location.altitude)
        .put("accuracy", if (location.hasAccuracy()) location.accuracy.toDouble() else -1.0)
        .put("verticalAccuracy", if (location.hasVerticalAccuracy()) location.verticalAccuracyMeters.toDouble() else -1.0)
        .put("speed", if (location.hasSpeed()) location.speed.toDouble() else -1.0)
        .put("course", if (location.hasBearing()) location.bearing.toDouble() else -1.0)
        .put("time", location.time / MILLISECONDS_PER_SECOND)
        .toString()

    /** The state of the default network, in the shape NWPath is built from. */
    fun networkStatus(): String = describeNetwork(connectivity.getNetworkCapabilities(connectivity.activeNetwork))

    private fun describeNetwork(capabilities: NetworkCapabilities?): String = JSONObject()
        .put("satisfied", capabilities?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true)
        .put("wifi", capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true)
        .put("cellular", capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) == true)
        .put("wired", capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true)
        .toString()

    private fun startNetwork(id: Int) {
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = reply(id, describeNetwork(capabilities))
            override fun onLost(network: Network) = reply(id, describeNetwork(null))
        }
        networkCallbacks[id] = callback
        connectivity.registerDefaultNetworkCallback(callback, mainThread)
        reply(id, networkStatus())
    }

    private fun stopNetwork(id: Int) {
        networkCallbacks.remove(id)?.let { connectivity.unregisterNetworkCallback(it) }
    }

    private companion object {
        const val PREFERENCES = "device_services"
        const val CHANNEL = "guest"
        const val GROUP_LOCATION = "location"
        const val GROUP_NOTIFICATIONS = "notifications"
        const val LOCATION_INTERVAL_MS = 1000L
        const val MILLISECONDS_PER_SECOND = 1000.0
    }
}
