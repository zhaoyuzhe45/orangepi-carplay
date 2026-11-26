package com.carplaybox.audio;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.view.*;
import android.widget.*;
import java.util.*;

/** Projection-friendly view of the active player's real queue; no fabricated playlist. */
public final class MediaQueueActivity extends Activity {
    private final Handler handler=new Handler();
    private List<MediaQueue.Entry> entries=Collections.emptyList();
    private long version;
    private int playing=-1;
    private int playbackStatus=-1;
    private TextView subtitle,empty;
    private ListView list;
    private Button previous,toggle,next;
    private QueueAdapter adapter;
    private final Runnable refresh=new Runnable(){public void run(){render();handler.postDelayed(this,1000);}};
    private void immersive(){getWindow().getDecorView().setSystemUiVisibility(0x1706);}
    @Override public void onCreate(Bundle state){
        super.onCreate(state);CarPlayServices.start(this);immersive();
        LinearLayout root=Ui.vertical(this);root.setBackgroundColor(Ui.BACKGROUND);
        root.setPadding(Ui.dp(this,24),Ui.dp(this,16),Ui.dp(this,24),Ui.dp(this,12));
        LinearLayout header=Ui.horizontal(this),heading=Ui.vertical(this);
        TextView title=Ui.title(this,"播放队列");title.setTextSize(26);heading.addView(title);
        subtitle=Ui.text(this,"等待播放器",14,Ui.MUTED);subtitle.setMaxLines(2);heading.addView(subtitle);
        header.addView(heading,Ui.weight());
        previous=control("上一首",88);toggle=control("播放",85);next=control("下一首",87);
        for(Button b:new Button[]{previous,toggle,next}){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(Ui.dp(this,80),Ui.dp(this,50));p.leftMargin=Ui.dp(this,8);header.addView(b,p);}
        root.addView(header,Ui.matchWrap());
        FrameLayout body=new FrameLayout(this);
        list=new ListView(this);list.setId(View.generateViewId());list.setDivider(new android.graphics.drawable.ColorDrawable(Ui.BACKGROUND));list.setDividerHeight(Ui.dp(this,6));
        list.setDrawSelectorOnTop(true);list.setSelector(outline());list.setItemsCanFocus(false);list.setPadding(0,Ui.dp(this,16),0,0);
        adapter=new QueueAdapter();list.setAdapter(adapter);
        list.setOnItemClickListener((parent,view,position,id)->select(position));
        empty=Ui.text(this,"先在音乐应用中开始播放\n播放器需要共享播放队列和选歌能力",18,Ui.MUTED);empty.setGravity(Gravity.CENTER);
        body.addView(list,new FrameLayout.LayoutParams(-1,-1));body.addView(empty,new FrameLayout.LayoutParams(-1,-1));list.setEmptyView(empty);
        root.addView(body,new LinearLayout.LayoutParams(-1,0,1));
        TextView hint=Ui.text(this,"旋钮左右选曲 · 按下播放 · 方向键上下移动",12,Ui.MUTED);hint.setPadding(0,Ui.dp(this,8),0,0);root.addView(hint);
        setContentView(root);list.requestFocus();
    }
    private GradientDrawable outline(){GradientDrawable d=new GradientDrawable();d.setColor(0x185baaff);d.setCornerRadius(Ui.dp(this,10));d.setStroke(Ui.dp(this,2),Ui.ACCENT);return d;}
    private Button control(String title,int key){Button b=Ui.button(this,title,v->{AppIntegrationService s=AppIntegrationService.current();if(s==null || !s.command(1,key))message("播放器暂不可用");});
        b.setTextColor(Ui.TEXT);b.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{android.R.attr.state_focused},new int[]{}},new int[]{0xff275584,Ui.PANEL}));return b;}
    private void message(String text){Toast.makeText(this,text,Toast.LENGTH_SHORT).show();}
    private void select(int index){AppIntegrationService s=AppIntegrationService.current();if(s==null || !s.queueSelect(version,index)){message("播放队列已更新，请重新选择");render();}}
    private void render(){
        AppIntegrationService s=AppIntegrationService.current();AppSnapshot snapshot=s==null?null:s.media();
        if(snapshot==null || SystemClock.uptimeMillis()-snapshot.sampled>5000){
            version=0;entries=Collections.emptyList();playing=-1;adapter.notifyDataSetChanged();subtitle.setText("等待播放器");return;
        }
        MediaQueue q=s.queue();long nextVersion=q.version();int index=snapshot.queueIndex;
        int oldStatus=playbackStatus;playbackStatus=snapshot.status;
        if(nextVersion!=version){
            long selectedId=list.getSelectedItemPosition()>=0 && list.getSelectedItemPosition()<entries.size()?entries.get(list.getSelectedItemPosition()).id:-1;
            entries=q.page(nextVersion,0,10000,SystemClock.uptimeMillis());version=nextVersion;playing=index;adapter.notifyDataSetChanged();
            int selected=-1;for(int i=0;i<entries.size();i++)if(entries.get(i).id==selectedId){selected=i;break;}
            if(!entries.isEmpty())list.setSelection(selected>=0?selected:Math.max(0,Math.min(index,entries.size()-1)));
        }else if(playing!=index || oldStatus!=playbackStatus){playing=index;adapter.notifyDataSetChanged();}
        subtitle.setText((snapshot.appName==null?"":snapshot.appName)+(entries.isEmpty()?" · 未共享可选队列":" · 共 "+entries.size()+" 首")+"\n"+(snapshot.title==null?"":snapshot.title));
        toggle.setText(snapshot.status==1?"暂停":"播放");
    }
    @Override protected void onResume(){super.onResume();immersive();handler.removeCallbacks(refresh);handler.post(refresh);}
    @Override protected void onPause(){handler.removeCallbacks(refresh);super.onPause();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)immersive();}
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        int key=event.getKeyCode();
        if(key==KeyEvent.KEYCODE_ESCAPE){if(event.getAction()==KeyEvent.ACTION_UP)finish();return true;}
        if(key==KeyEvent.KEYCODE_DPAD_LEFT || key==KeyEvent.KEYCODE_DPAD_RIGHT){
            if(event.getAction()!=KeyEvent.ACTION_DOWN)return true;
            int direction=key==KeyEvent.KEYCODE_DPAD_LEFT?-1:1;View focus=getCurrentFocus();
            if(focus==list){int selected=list.getSelectedItemPosition();int to=(selected<0?0:selected+direction);
                if(to<0)next.requestFocus();else if(to<entries.size())list.setSelection(to);
            }else{
                Button[] buttons={previous,toggle,next};int i=0;while(i<3 && buttons[i]!=focus)i++;
                int to=i+direction;if(to>=3){list.requestFocus();if(!entries.isEmpty())list.setSelection(0);}else buttons[Math.max(0,to)].requestFocus();
            }return true;
        }
        if((key==KeyEvent.KEYCODE_DPAD_DOWN || key==KeyEvent.KEYCODE_DPAD_UP) && getCurrentFocus()!=list && !entries.isEmpty()){
            if(event.getAction()==KeyEvent.ACTION_DOWN){list.requestFocus();list.setSelection(0);}return true;
        }
        return super.dispatchKeyEvent(event);
    }
    private final class QueueAdapter extends BaseAdapter {
        public int getCount(){return entries.size();}public Object getItem(int p){return entries.get(p);}public long getItemId(int p){return entries.get(p).id;}
        public View getView(int position,View recycled,ViewGroup parent){
            LinearLayout row;TextView title,detail;
            if(recycled==null){row=Ui.vertical(MediaQueueActivity.this);row.setPadding(Ui.dp(MediaQueueActivity.this,16),Ui.dp(MediaQueueActivity.this,9),Ui.dp(MediaQueueActivity.this,16),Ui.dp(MediaQueueActivity.this,9));
                title=Ui.text(MediaQueueActivity.this,"",18,Ui.TEXT);title.setSingleLine(true);title.setEllipsize(android.text.TextUtils.TruncateAt.END);title.setTypeface(Typeface.DEFAULT,Typeface.BOLD);row.addView(title);
                detail=Ui.text(MediaQueueActivity.this,"",13,Ui.MUTED);detail.setSingleLine(true);detail.setEllipsize(android.text.TextUtils.TruncateAt.END);row.addView(detail);
            }else{row=(LinearLayout)recycled;title=(TextView)row.getChildAt(0);detail=(TextView)row.getChildAt(1);}
            String state=playbackStatus==1?"播放中":playbackStatus==2?"已暂停":"当前曲目";
            MediaQueue.Entry e=entries.get(position);title.setText((position+1)+"   "+(e.title.isEmpty()?"未命名曲目":e.title)+(playing==position?"   · "+state:""));title.setTextColor(playing==position?Ui.ACCENT:Ui.TEXT);
            detail.setText(e.artist+(e.artist.isEmpty()||e.album.isEmpty()?"":" · ")+e.album);
            GradientDrawable background=new GradientDrawable();background.setColor(Ui.PANEL);background.setCornerRadius(Ui.dp(MediaQueueActivity.this,10));row.setBackground(background);return row;
        }
    }
}
