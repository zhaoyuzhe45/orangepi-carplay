package com.carplaybox.audio;

import android.app.Activity;
import android.os.Handler;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.*;
import java.util.*;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Five contacts per page keep comfortable row heights at 800 by 480 dp. */
final class ContactsPanel implements AutoCloseable {
    final LinearLayout root;
    private final PhoneActivity activity;
    private final TextView status,pageLabel,empty;
    private final EditText search;
    private final Button[] rows=new Button[5];
    private final Button previous,next,dialer,close,sync,pair;
    private final Handler handler=new Handler();
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private PhoneNavigation navigation=new PhoneNavigation();
    private List<PhoneContacts.Entry> contacts=Collections.emptyList();
    private boolean active,loading,closed;
    private int page,generation;
    private String device="",result="手机连接后自动同步通讯录";
    ContactsPanel(PhoneActivity a) {
        activity=a;root=Ui.vertical(a);root.setBackgroundColor(PhoneStyle.BG);
        root.setPadding(Ui.dp(a,20),Ui.dp(a,12),Ui.dp(a,20),Ui.dp(a,8));
        LinearLayout header=Ui.horizontal(a);header.addView(PhoneStyle.text(a,"通讯录",28,PhoneStyle.TEXT),Ui.weight());
        pair=PhoneStyle.button(a,"配对手机",PhoneStyle.KEY,v->a.pairPhone());pair.setTag("pair-phone");header.addView(pair,headerButton(a,112));
        dialer=PhoneStyle.button(a,"拨号键盘",PhoneStyle.KEY,v->a.showDialer(null));dialer.setTag("tab-dialer");header.addView(dialer,headerButton(a,120));
        close=PhoneStyle.button(a,"关闭",PhoneStyle.KEY,v->a.finish());header.addView(close,headerButton(a,76));root.addView(header,Ui.matchWrap());
        LinearLayout tools=Ui.horizontal(a);search=Ui.number(a,"按姓名或号码搜索");search.setInputType(android.text.InputType.TYPE_CLASS_TEXT);search.setSelectAllOnFocus(false);
        search.setFocusableInTouchMode(true);search.setTag("contact-search");search.setTextSize(17);search.setSingleLine(true);
        search.setTextColor(PhoneStyle.TEXT);search.setHintTextColor(PhoneStyle.MUTED);
        search.setPadding(Ui.dp(a,16),0,Ui.dp(a,16),0);search.setBackground(PhoneStyle.shape(a,PhoneStyle.CARD,0));
        tools.addView(search,new LinearLayout.LayoutParams(0,Ui.dp(a,48),1));
        status=PhoneStyle.text(a,result,14,PhoneStyle.MUTED);
        sync=PhoneStyle.button(a,"立即刷新",PhoneStyle.GREEN,v->{
            PhoneState s=PhoneBackend.LIVE.state();
            result=PhoneBackend.LIVE.sync(s==null?"":s.selected);status.setText(result);load();
        });sync.setTag("sync-contacts");tools.addView(sync,headerButton(a,120));
        LinearLayout.LayoutParams toolRow=Ui.matchWrap();toolRow.topMargin=Ui.dp(a,14);root.addView(tools,toolRow);
        status.setTextSize(13);status.setSingleLine(true);status.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams statusRow=Ui.matchWrap();statusRow.topMargin=Ui.dp(a,8);statusRow.bottomMargin=Ui.dp(a,6);root.addView(status,statusRow);
        FrameLayout content=new FrameLayout(a);LinearLayout list=Ui.vertical(a);
        for(int i=0;i<rows.length;i++) {
            Button row=PhoneStyle.button(a,"",PhoneStyle.CARD,v->{});row.setGravity(android.view.Gravity.START|android.view.Gravity.CENTER_VERTICAL);
            row.setTextSize(17);row.setMaxLines(2);row.setTag("contact-"+i);rows[i]=row;
            LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,0,1);p.setMargins(0,Ui.dp(a,3),0,Ui.dp(a,3));list.addView(row,p);
        }
        content.addView(list,new FrameLayout.LayoutParams(-1,-1));empty=PhoneStyle.text(a,"等待手机通讯录\n\n连接后自动同步，请在手机上允许共享通讯录。",20,PhoneStyle.MUTED);
        empty.setGravity(android.view.Gravity.CENTER);content.addView(empty,new FrameLayout.LayoutParams(-1,-1));root.addView(content,new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout footer=Ui.horizontal(a);previous=PhoneStyle.button(a,"上一页",PhoneStyle.KEY,v->{page--;render();});footer.addView(previous,PhoneStyle.row(a));
        pageLabel=PhoneStyle.text(a,"1 / 1",15,PhoneStyle.MUTED);pageLabel.setGravity(android.view.Gravity.CENTER);footer.addView(pageLabel,Ui.weight());
        next=PhoneStyle.button(a,"下一页",PhoneStyle.KEY,v->{page++;render();});footer.addView(next,PhoneStyle.row(a));root.addView(footer,Ui.matchWrap());
        search.addTextChangedListener(new TextWatcher() { public void beforeTextChanged(CharSequence s,int start,int count,int after){}public void onTextChanged(CharSequence s,int start,int before,int count){page=0;render();}public void afterTextChanged(Editable e){} });
        render();dialer.requestFocus();
    }
    private static LinearLayout.LayoutParams headerButton(Activity a,int width) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(Ui.dp(a,width),Ui.dp(a,48));p.leftMargin=Ui.dp(a,10);return p;
    }
    void start() { active=true;load();handler.removeCallbacks(poll);handler.postDelayed(poll,2500); }
    void message(String text) { result=text;status.setText(text); }
    void stop() { active=false;generation++;handler.removeCallbacks(poll);navigation.cancel(); }
    private final Runnable poll=new Runnable() { public void run(){if(active){load();handler.postDelayed(this,2500);}} };
    private void load() {
        PhoneState s=PhoneBackend.LIVE.state();String selected=s==null?"":s.selected;
        if(!selected.equals(device)) { device=selected;generation++;contacts=Collections.emptyList();page=0;render(); }
        if(loading || closed)return;loading=true;final int token=generation;final String target=device;
        worker.execute(()-> {
            List<PhoneContacts.Entry> data;String error="";
            try { data=PhoneBackend.LIVE.contacts(target); }
            catch(SecurityException e) { data=Collections.emptyList();error="请允许 CarPlay 控制台读取联系人，并确认电话应用来自配套安装包"; }
            catch(Exception e) { data=Collections.emptyList();error="读取通讯录失败："+e.getClass().getSimpleName(); }
            final List<PhoneContacts.Entry> answer=data;final String failure=error;
            handler.post(()-> {
                loading=false;if(!active || closed || token!=generation || !target.equals(device))return;
                PhoneState latest=PhoneBackend.LIVE.state();String response=PhoneBackend.LIVE.consumeMessage();String info=!failure.isEmpty()?failure:target.isEmpty()?"请先切换到拨号键盘，连接手机":response!=null?response:latest==null?"电话服务连接中":latest.contactsStatus;
                status.setText(answer.isEmpty()?info:"已读取 "+answer.size()+" 个号码 · "+info);
                boolean changed=contacts.size()!=answer.size();
                for(int i=0;!changed && i<contacts.size();i++)changed=!contacts.get(i).name.equals(answer.get(i).name)||!contacts.get(i).number.equals(answer.get(i).number);
                if(changed){contacts=answer;render();}
            });
        });
    }
    private void render() {
        navigation.cancel();navigation=new PhoneNavigation();navigation.add(pair);navigation.add(dialer);navigation.add(close);navigation.add(search);navigation.add(sync);
        List<PhoneContacts.Entry> visible=PhoneContacts.filter(contacts,search.getText().toString());int count=PhoneContacts.pageCount(visible.size(),rows.length);page=PhoneContacts.pageIndex(page,count);
        for(int i=0;i<rows.length;i++) {
            int index=page*rows.length+i;Button row=rows[i];boolean show=index<visible.size();row.setVisibility(show?View.VISIBLE:View.INVISIBLE);
            if(show) {
                PhoneContacts.Entry e=visible.get(index);String target=device;
                row.setText(e.name+"    "+e.number);row.setOnClickListener(v->{
                    PhoneState s=PhoneBackend.LIVE.state();
                    if(s==null || !s.selected.equals(target)) { status.setText("手机已切换，请重新选择联系人");return; }
                    activity.showDialer(e.number);
                });navigation.add(row);
            }
        }
        empty.setVisibility(visible.isEmpty()?View.VISIBLE:View.GONE);
        empty.setText(contacts.isEmpty()?"等待手机通讯录\n\n连接后自动同步，请在手机上允许共享通讯录。":"没有匹配的联系人");
        PhoneStyle.enabled(previous,page>0);PhoneStyle.enabled(next,page+1<count);pageLabel.setText((page+1)+" / "+count);
        navigation.add(previous);navigation.add(next);
    }
    boolean key(KeyEvent e) {
        // The car knob emits left/right even over the search box; keep focus traversal reachable.
        if(search.hasFocus() && (e.getKeyCode()==21 || e.getKeyCode()==22)) {
            if(e.getAction()==KeyEvent.ACTION_DOWN)navigation.move(e.getKeyCode()==21?-1:1);return true;
        }
        return navigation.key(e);
    }
    boolean motion(MotionEvent e) { return navigation.motion(e); }
    @Override public void close() { stop();closed=true;worker.shutdownNow(); }
}
