package com.virtualcam.manager.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.virtualcam.manager.data.PrerequisiteStatus

@Composable
fun FailureDoctorCard(
    status: PrerequisiteStatus?,
    enabled: Boolean,
    live: Boolean,
    partial: Boolean,
    hook: String,
    frames: Int,
    hits: Int,
    rootOk: Boolean,
    busy: Boolean,
    onProbe: () -> Unit,
    onRefresh: () -> Unit,
) {
    if (!enabled || status == null) return
    val hint = when {
        hook.startsWith("surface_playing") || hook.startsWith("texture_swapped") ||
            hook.startsWith("display_swapped") ->
            "Camera1 surface path is live. Preview should show virtual.mp4 in Camera1 apps."
        hook.startsWith("jni_hooks_ready") || hook.startsWith("jni_camera_patched") ->
            "JNI hooks installed. Open a camera app so setPreviewTexture can fire."
        hook.startsWith("jni_hook_pending") || hook.startsWith("gl_hook_pending") ->
            "ShadowHook did not load in the target process. Reflash the CI Magisk zip and reboot."
        hook.startsWith("surface_player_fail") || hook.startsWith("surface_no_video") ->
            "Surface swap happened but MediaPlayer could not play virtual.mp4. Re-import the video."
        hook.startsWith("oes_ready") || hook.startsWith("gl_bind_redir") ->
            "GL path is intercepting textures. Some Camera2 apps still need the surface path."
        live -> "Native path is producing frames. If the app still shows the real camera, it is Camera2-only."
        partial -> "Hooks are installing or waiting for the app to open the camera."
        else -> "Enable VirtualCam, then open a camera app. Status updates automatically."
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.7f)
        )
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Doctor", style = MaterialTheme.typography.titleSmall)
            Text(hint, style = MaterialTheme.typography.bodySmall)
            Text(
                "hook=$hook  frames=$frames  binds=$hits  root=$rootOk",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            TextButton(onClick = { if (!busy) { onRefresh(); onProbe() } }) { Text("Refresh status") }
        }
    }
}
