package com.koldecomp.game;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * One app, more than one game (src/kol_region.h kCarts, the same table): the
 * USA cartridge and the Japanese one, each with its own native library, since
 * each ROM has its own recompiled code. The player gives either ROM, and
 * MainActivity loads the library of the game to play. A process holds one of
 * them: changing game is a restart.
 *
 * Each game keeps what it made in its own folder of files/: its ROM, its save
 * and slots, its numbered states, its mod. The USA game's folder is files/
 * itself, where its files have always been (an update moves nothing); the
 * Japanese one's is files/japan. The BIOS and the settings are every game's.
 */
final class Game {
    static final int USA = 0, JAPAN = 1, COUNT = 2;
    static final String[] KEY = {"usa", "japan"};
    static final String[] NAME = {"Tactics Ogre: The Knight of Lodis",
                                  "Tactics Ogre Gaiden: The Knight of Lodis (Japan)"};
    static final String[] VERSION = {"USA", "Japanese"};
    static final String[] SHA1 = {"85a46320a31f16f5c6dda27fc094edd078dbe43d",
                                  "ec257d645c601e439f1ab34d9b598c0443aec3da"};
    private static final String[] FOLDER = {"", "japan"};
    private static final String[] LIBRARY = {"main", "main_jp"};
    // main_android.cpp looks for exactly this name, in the game's folder.
    static final String ROM_NAME = "totkol.gba";
    private static final String CHOICE = "game.cfg";

    /** The game whose library this process loaded (MainActivity), or -1. */
    static int loaded = -1;
    /** The game the setup screen's boxes are for, while it is up; else -1. */
    static int selected = -1;

    private Game() {}

    static File dir(Context c, int game) {
        return FOLDER[game].isEmpty() ? c.getFilesDir() : new File(c.getFilesDir(), FOLDER[game]);
    }

    static File rom(Context c, int game) { return new File(dir(c, game), ROM_NAME); }

    static String library(int game) { return LIBRARY[game]; }

    /** Whether this app holds the game's library (the Japanese one is built
     *  only where that game's recompiled code is: jni/CMakeLists.txt). The
     *  libraries are unpacked at install (useLegacyPackaging). */
    static boolean hasProgram(Context c, int game) {
        if (game == USA) return true;
        return new File(c.getApplicationInfo().nativeLibraryDir, "lib" + LIBRARY[game] + ".so").exists();
    }

    /** How many games this app can play. */
    static int programs(Context c) {
        int n = 0;
        for (int g = 0; g < COUNT; ++g) if (hasProgram(c, g)) ++n;
        return n;
    }

    /** Which game's ROM has this SHA-1; -1: neither's. */
    static int ofSha1(String sha) {
        for (int g = 0; g < COUNT; ++g) if (SHA1[g].equalsIgnoreCase(sha)) return g;
        return -1;
    }

    /** A ROM file in the game's folder, and its library in the app. (The
     *  file's contents are checked where it comes in and at each start:
     *  SetupActivity.) */
    static boolean playable(Context c, int game) {
        return hasProgram(c, game) && rom(c, game).isFile();
    }

    /** The game to play: the one last chosen when it can be played, else the
     *  first that can; -1: none. */
    static int current(Context c) {
        String key = "";
        try {
            byte[] b = java.nio.file.Files.readAllBytes(new File(c.getFilesDir(), CHOICE).toPath());
            key = new String(b, StandardCharsets.UTF_8).trim();
        } catch (Exception ignored) {
        }
        for (int g = 0; g < COUNT; ++g) if (KEY[g].equals(key) && playable(c, g)) return g;
        for (int g = 0; g < COUNT; ++g) if (playable(c, g)) return g;
        return -1;
    }

    /** The choice, kept in files/game.cfg. */
    static void choose(Context c, int game) {
        try (FileOutputStream o = new FileOutputStream(new File(c.getFilesDir(), CHOICE))) {
            o.write((KEY[game] + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {
        }
    }

    /** The game a file of "the game's" is for, wherever it is asked: the
     *  running game's, else the setup screen's, else the one that would
     *  start, else the first. */
    static int active(Context c) {
        if (loaded >= 0) return loaded;
        if (selected >= 0) return selected;
        final int g = current(c);
        return g >= 0 ? g : USA;
    }

    static File activeDir(Context c) { return dir(c, active(c)); }
}
