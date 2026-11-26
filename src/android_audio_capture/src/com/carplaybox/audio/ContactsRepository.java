package com.carplaybox.audio;

import android.content.Context;
import android.content.res.Resources;
import android.database.Cursor;
import android.provider.ContactsContract;
import java.util.*;

/** Reads only the selected phone's PBAP account, excluding unrelated local/Google contacts. */
final class ContactsRepository {
    static List<PhoneContacts.Entry> read(Context context,String device) throws Exception {
        if(device.isEmpty())return Collections.emptyList();
        Resources bluetooth=context.getPackageManager().getResourcesForApplication("com.android.bluetooth");
        String account=bluetooth.getString(bluetooth.getIdentifier("pbap_account_type","string","com.android.bluetooth"));
        String[] columns={ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME,ContactsContract.CommonDataKinds.Phone.NUMBER};
        String where=ContactsContract.RawContacts.ACCOUNT_NAME+"=? AND "+ContactsContract.RawContacts.ACCOUNT_TYPE+"=?";
        ArrayList<PhoneContacts.Entry> rows=new ArrayList<>();
        try(Cursor c=context.getContentResolver().query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI,columns,where,new String[]{device,account},null)) {
            if(c==null)throw new IllegalStateException("contacts provider unavailable");
            while(c.moveToNext())rows.add(new PhoneContacts.Entry(c.getString(0),c.getString(1)));
        }
        return PhoneContacts.normalize(rows);
    }
}
