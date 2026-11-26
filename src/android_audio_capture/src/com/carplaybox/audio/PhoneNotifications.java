package com.carplaybox.audio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.util.Log;

/** Notification intents only open UI; they cannot dial or apply stale call actions. */
final class PhoneNotifications implements AutoCloseable {
    private static final int ID=45;
    private final Context context;
    private final NotificationManager manager;
    private final PhoneUiModel.Incoming incoming=new PhoneUiModel.Incoming();
    private String last="";
    PhoneNotifications(Context c) {
        context=c;manager=c.getSystemService(NotificationManager.class);
        NotificationChannel channel=new NotificationChannel("carplay_phone","手机电话",NotificationManager.IMPORTANCE_HIGH);
        channel.setDescription("来电与当前通话");channel.setSound(null,null);channel.enableVibration(false);
        manager.createNotificationChannel(channel);manager.cancel(ID);
    }
    void update(PhoneState s) {
        if(s==null) { clear();return; }
        boolean known=PhoneUiModel.fresh(s.connected,s.knownCalls,s.sampled,SystemClock.uptimeMillis());
        String fresh=incoming.update(s.selected,known,s.calls);
        if(!known || s.calls.isEmpty()) { clear();return; }
        BluetoothCalls.Call call=s.calls.get(0);
        String key=s.selected+"/"+call.uuid+"/"+call.state+"/"+s.calls.size();
        if(key.equals(last))return;
        Intent open=PhoneBridgeContract.openPhone().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent content=PendingIntent.getActivity(context,ID,open,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder n=new Notification.Builder(context,"carplay_phone").setSmallIcon(android.R.drawable.sym_action_call)
            .setContentTitle(call.label()).setContentText("打开电话查看和操作").setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PRIVATE).setOngoing(true).setOnlyAlertOnce(true).setContentIntent(content);
        Intent card=null;
        if(!fresh.isEmpty()) {
            card=PhoneBridgeContract.incoming().putExtra("device",s.selected).putExtra("uuid",fresh)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP);
            n.setFullScreenIntent(PendingIntent.getActivity(context,ID+1,card,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_IMMUTABLE),true);
        }
        try {
            manager.notify(ID,n.build());last=key;
            // This target is Android 8.1. Direct presentation also covers projected fullscreen apps
            // where a heads-up notification may be outside the captured surface.
            if(card!=null)context.startActivity(card);
        } catch(RuntimeException error) { Log.w("CarPlayPhoneUi","Cannot present call card: "+error.getClass().getSimpleName()); }
    }
    private void clear() { if(!last.isEmpty())manager.cancel(ID);last=""; }
    @Override public void close() { manager.cancel(ID);last=""; }
}
