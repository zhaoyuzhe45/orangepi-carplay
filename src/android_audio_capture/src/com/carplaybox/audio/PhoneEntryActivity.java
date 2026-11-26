package com.carplaybox.audio;
import android.app.Activity;
import android.os.Bundle;
import android.widget.Toast;
/** Retains old explicit shortcuts without retaining a second telephone UI. */
public final class PhoneEntryActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try{startActivity(PhoneBridgeContract.openPhone());}
        catch(RuntimeException e){Toast.makeText(this,"请安装车载电话应用",Toast.LENGTH_LONG).show();}
        finish();
    }
}
