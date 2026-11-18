package com.wearosports.smbjr;

import java.io.File;
import java.util.Locale;

/** Game specific rules for the Wi-Fi ROM transfer (GBA cartridge dump). */
final class RomInboxConfig {
    private RomInboxConfig() {}

    static final String GAME = "Super Recomp Ball Jr.";
    static final int ACCENT = 0xFFFAC72E;
    static final String ACCEPT = ".gba,.bin,application/octet-stream";
    /** Largest GBA cartridge is 32 MB. */
    static final long MAX_UPLOAD = 32L << 20;

    static String hint(String lang) {
        return "pt".equals(lang)
                ? "Envie a ROM .gba do seu cartucho de Super Monkey Ball Jr. (USA). Arquivos .zip não funcionam: descompacte antes."
                : "Send the .gba ROM dumped from your Super Monkey Ball Jr. (USA) cartridge. .zip files do not work: extract it first.";
    }

    /** Name saved in the drop folder (LauncherActivity imports any .gba there), or null to refuse. */
    static String destName(String uploadName, long size) {
        if (size < 0xC0 || size > MAX_UPLOAD) return null;
        String n = uploadName == null ? "" : uploadName.toLowerCase(Locale.ROOT);
        if (n.endsWith(".zip") || n.endsWith(".7z") || n.endsWith(".rar")) return null;
        return "wifi-upload.gba";
    }

    /** null if the file is a GBA ROM, otherwise a Page text key. */
    static String validate(File f, String uploadName) {
        return RomStore.isSmbJr(RomStore.readHeader(f)) ? null : "notRom";
    }
}
