package com.wearosports.smbjr;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;

/** Private copy of the user's own ROM + header validation. Nothing is ever bundled. */
public final class RomStore {
    public static final String EXPECTED_TITLE = "MONKEYBALLJR";
    public static final String EXPECTED_CODE = "ALUE";

    private RomStore() {}

    public static File romFile(Context c) {
        return new File(c.getFilesDir(), "rom.gba");
    }

    /** Folder reachable with `adb push` without any storage permission. */
    public static File dropDir(Context c) {
        File d = c.getExternalFilesDir(null);
        return d != null ? d : c.getFilesDir();
    }

    public static boolean hasRom(Context c) {
        File f = romFile(c);
        return f.isFile() && f.length() >= 0x200;
    }

    /** Looks for any .gba dropped via adb into the app's external files dir. */
    public static File findDropped(Context c) {
        File[] files = dropDir(c).listFiles();
        if (files == null) return null;
        for (File f : files) {
            if (f.isFile() && f.getName().toLowerCase().endsWith(".gba")) return f;
        }
        return null;
    }

    /** Folder inside the APK where debug builds carry the ROM (wear/dev-rom/, see wear/build.gradle). */
    static final String BUNDLED_DIR = "devrom";

    /**
     * Debug builds only: imports the .gba packed in the APK. The APK copy cannot be removed;
     * the game keeps just its private rom.gba. Returns false when the build carries no ROM.
     */
    public static boolean importBundled(Context c) throws IOException {
        String[] names = c.getAssets().list(BUNDLED_DIR);
        if (names == null) return false;
        for (String n : names) {
            if (!n.toLowerCase().endsWith(".gba")) continue;
            String warn = importFrom(c, c.getAssets().open(BUNDLED_DIR + "/" + n));
            if (warn != null && !hasRom(c)) throw new IOException(warn);
            return hasRom(c);
        }
        return false;
    }

    /** Deletes every .gba (and partial upload) left in the drop folder. */
    public static void deleteDropped(Context c) {
        File[] files = dropDir(c).listFiles();
        if (files == null) return;
        for (File f : files) {
            String n = f.getName().toLowerCase();
            if (f.isFile() && (n.endsWith(".gba") || n.equals("upload.part"))) f.delete();
        }
    }

    /** Returns null on success, or a human readable warning/error. */
    public static String importFrom(Context c, InputStream in) throws IOException {
        File tmp = new File(c.getFilesDir(), "rom.tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            in.close();
        }
        String header = readHeader(tmp);
        if (header == null) {
            tmp.delete();
            return "Arquivo invalido (nao e uma ROM de GBA). Se for .zip, descompacte antes.";
        }
        // The native shell only plays Super Monkey Ball Jr. (any region): refuse other games and
        // keep the current ROM untouched.
        if (!isSmbJr(header)) {
            tmp.delete();
            return "ROM de outro jogo (" + header + "). Escolha Super Monkey Ball Jr.";
        }
        File dst = romFile(c);
        dst.delete();
        if (!tmp.renameTo(dst)) throw new IOException("rename failed");
        return null;
    }

    public static String importFrom(Context c, Uri uri) throws IOException {
        InputStream in = c.getContentResolver().openInputStream(uri);
        if (in == null) throw new IOException("cannot open " + uri);
        return importFrom(c, in);
    }

    public static String importFrom(Context c, File f) throws IOException {
        return importFrom(c, new FileInputStream(f));
    }

    /** True for any region of the game (title MONKEYBALLJR; USA is ALUE). */
    static boolean isSmbJr(String header) {
        return header != null && header.startsWith(EXPECTED_TITLE + "/");
    }

    /** "TITLE/CODE" or null if the Nintendo logo check byte / size is wrong. */
    static String readHeader(File f) {
        if (f.length() < 0xC0) return null;
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            byte[] h = new byte[0xC0];
            raf.readFully(h);
            if ((h[0xB2] & 0xFF) != 0x96) return null;  // fixed value in every GBA header
            String title = new String(h, 0xA0, 12, "US-ASCII").trim().replace("\0", "");
            String code = new String(h, 0xAC, 4, "US-ASCII");
            return title + "/" + code;
        } catch (IOException e) {
            return null;
        }
    }
}
