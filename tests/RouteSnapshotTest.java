package com.carplaybox.audio;
public final class RouteSnapshotTest {
    static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
    public static void main(String[] args)throws Exception {
        check(RouteSnapshot.maneuver(2)==1 && RouteSnapshot.maneuver(3)==2,"verified left/right");
        check(RouteSnapshot.maneuver(999)==255,"unknown icon omitted");
        RouteSnapshot p=new RouteSnapshot();p.update(0,2,3,1000,60,50,"road","next","dest",1000);
        byte[] a=p.encode(1500);check(a[12]==1 && a[13]==1 && a[15]==3,"active state");
        check(p.encode(11001)[12]==(byte)255,"timeout clears old maneuver");
        p.update(2,2,3,1000,60,50,"road","next","dest",12000);
        check(p.encode(12000)[12]==0,"cruise has no route");
        p.update(0,2,3,-1,60,50,"road","next","dest",13000);
        check(p.encode(13000)[12]==0,"invalid update does not overwrite known no-route");
        System.out.println("Navigation model bounds/expiry tests: PASS");
    }
}
