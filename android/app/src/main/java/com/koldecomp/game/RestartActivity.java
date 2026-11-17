package com.koldecomp.game;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Restarts the app: the game reads the ROM, the BIOS and the mod only at start,
 * and its native runtime cannot start twice in one process. This activity runs
 * in its own process (":restart" in the manifest), so the game's process can
 * end at once; here it is made sure to be gone, the app starts again from
 * SetupActivity (which goes on to the game when the ROM is there), and this
 * process ends too. Started while the app is in front, so Android lets it
 * start the next activity.
 */
public class RestartActivity extends Activity {
    private static final String EXTRA_PID = "pid";

    /** Ends the game's process and starts the app again. */
    static void restart(Activity from) {
        Intent i = new Intent(from, RestartActivity.class);
        i.putExtra(EXTRA_PID, Process.myPid());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        from.startActivity(i);
        from.finishAffinity();
        Runtime.getRuntime().exit(0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Process.killProcess(getIntent().getIntExtra(EXTRA_PID, -1));
        Intent i = new Intent(this, SetupActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
