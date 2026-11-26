package com.carplaybox.audio;

/** Read-only framework-to-HAL installation check; does not open a microphone. */
public final class SharedMicrophoneStatusProbe {
    public static void main(String[] args) throws Exception {
        String value = (String) Class.forName("android.media.AudioSystem")
                .getMethod("getParameters", String.class)
                .invoke(null, "carplay_shared_mic_version");
        System.out.println("HAL parameters: " + value);
        boolean shared = "carplay_shared_mic_version=1".equals(value);
        boolean expectStock = args.length > 0 && "stock".equals(args[0]);
        if (expectStock && !"".equals(value)) System.exit(1);
        if (shared == expectStock) System.exit(1);
        System.out.println(expectStock ? "STOCK_MIC_HAL_READY" : "SHARED_MIC_HAL_READY");
    }
}
