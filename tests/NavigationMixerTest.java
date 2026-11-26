package com.carplaybox.audio;
import java.util.Arrays;
public final class NavigationMixerTest {
    static void check(boolean value,String reason){if(!value)throw new AssertionError(reason);}
    static short[] signal(int value){short[] p=new short[1920];Arrays.fill(p,(short)value);return p;}
    public static void main(String[] args){
        NavigationMixer m=new NavigationMixer();short[] media=signal(10000),nav=signal(0),out=new short[1920];
        m.mix(media,nav,out,0);check(Arrays.equals(media,out),"pure media unchanged");
        Arrays.fill(nav,(short)5000);
        m.mix(media,nav,out,20);m.mix(media,nav,out,40);
        check(out[1919]>=7498 && out[1919]<=7502,"navigation ducks media to quarter gain");
        check(m.navigationActive(),"speech activity");
        Arrays.fill(nav,(short)0);m.mix(media,nav,out,100);
        check(out[1919]>=2498 && out[1919]<=2502,"hold between speech words");
        for(int i=0;i<40;i++)m.mix(media,nav,out,400+i*20);
        check(Arrays.equals(media,out) && !m.navigationActive(),"smooth restore after speech");
        Arrays.fill(nav,(short)20);m.mix(media,nav,out,1400);
        check(out[1919]==10020 && !m.navigationActive(),"low noise does not duck");
        m=new NavigationMixer();media=signal(32000);nav=signal(32000);m.mix(media,nav,out,0);
        check(out[0]==32767,"positive saturation instead of overflow");
        media=signal(-32000);nav=signal(-32000);m.mix(media,nav,out,20);
        check(out[0]==-32768,"negative saturation instead of overflow");
        m.reset();media=signal(1000);nav=signal(0);m.mix(media,nav,out,0);
        check(Arrays.equals(media,out),"session reset clears ducking");
        m.reset();media=signal(12000);nav=signal(0);
        m.mix(media,nav,out,1800);
        m.mix(media,nav,out,1820);
        check(Arrays.equals(media,out),"background recorder must not change music samples");
        media=signal(3000);m.mix(media,nav,out,1840);
        check(Arrays.equals(media,out),"pass through volume already ducked by the player");
        media=signal(12000);m.mix(media,nav,out,1860);
        check(Arrays.equals(media,out),"no extra gain ramp when the app restores playback");
        boolean rejected=false;try{m.mix(new short[3],new short[3],new short[3],0);}catch(IllegalArgumentException e){rejected=true;}
        check(rejected,"stereo frame bounds");
        System.out.println("Navigation mixing, ducking, recovery and saturation: PASS");
    }
}
