package com.retrosound.receiver;

import android.app.Activity;
import android.os.Bundle;
import android.os.PowerManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import java.io.*;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Enumeration;

public class MainActivity extends Activity {
    private TextView status, format, ip;
    private Button toggle;
    private volatile boolean listening=true;
    private ServerSocket server;
    private Thread worker;
    private PowerManager.WakeLock wakeLock;
    private static final int PORT=49152;

    @Override public void onCreate(Bundle b){ super.onCreate(b); setContentView(R.layout.activity_main);
        status=(TextView)findViewById(R.id.statusText); format=(TextView)findViewById(R.id.formatText); ip=(TextView)findViewById(R.id.ipText); toggle=(Button)findViewById(R.id.toggleButton);
        ip.setText("IP: "+localIp());
        PowerManager pm=(PowerManager)getSystemService(POWER_SERVICE); wakeLock=pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"RetroSound:Audio"); wakeLock.acquire();
        toggle.setOnClickListener(new View.OnClickListener(){ public void onClick(View v){ if(listening) stopServer(); else startServer(); }});
        startServer();
    }

    private void ui(final String s, final String f){ runOnUiThread(new Runnable(){ public void run(){ status.setText(s); if(f!=null) format.setText(f); }}); }
    private void startServer(){ listening=true; toggle.setText("STOP LISTENING"); worker=new Thread(new Runnable(){ public void run(){ loop(); }},"RetroSoundServer"); worker.start(); }
    private void stopServer(){ listening=false; try{if(server!=null)server.close();}catch(Exception ignored){} toggle.setText("START LISTENING"); ui("Stopped",null); }

    private void loop(){
        try{
            server=new ServerSocket(); server.setReuseAddress(true); server.bind(new InetSocketAddress(PORT)); ui("Listening on port "+PORT,"Waiting for PC");
            while(listening){ Socket s=null; try{ s=server.accept(); s.setTcpNoDelay(true); playClient(s); } catch(IOException e){ if(listening) ui("Connection error: "+e.getMessage(),null); } finally{ if(s!=null)try{s.close();}catch(Exception ignored){} }
            }
        }catch(IOException e){ if(listening) ui("Server error: "+e.getMessage(),null); }
    }

    private void playClient(Socket sock) throws IOException {
        InputStream in=new BufferedInputStream(sock.getInputStream(),32768);
        byte[] h=new byte[14]; readFully(in,h);
        if(h[0]!='R'||h[1]!='S'||h[2]!='N'||h[3]!='D') throw new IOException("Bad RetroSound header");
        ByteBuffer bb=ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
        bb.position(4); int version=bb.getShort()&0xffff; int rate=bb.getInt(); int bits=bb.get()&0xff; int channels=bb.get()&0xff; int bufferMs=bb.getShort()&0xffff;
        if(version!=1) throw new IOException("Unsupported protocol");
        int chMask=(channels==1)?AudioFormat.CHANNEL_OUT_MONO:AudioFormat.CHANNEL_OUT_STEREO;
        int enc=(bits==8)?AudioFormat.ENCODING_PCM_8BIT:AudioFormat.ENCODING_PCM_16BIT;
        int min=AudioTrack.getMinBufferSize(rate,chMask,enc); if(min<=0) throw new IOException("Format unsupported by device");
        int wanted=Math.max(min,(rate*channels*(bits/8)*bufferMs)/1000);
        AudioTrack track=new AudioTrack(AudioManager.STREAM_MUSIC,rate,chMask,enc,wanted,AudioTrack.MODE_STREAM);
        if(track.getState()!=AudioTrack.STATE_INITIALIZED){ track.release(); throw new IOException("AudioTrack init failed"); }
        ui("Connected: "+sock.getInetAddress().getHostAddress(),rate+" Hz / "+bits+"-bit / "+(channels==2?"Stereo":"Mono")+" / "+bufferMs+" ms");
        byte[] buf=new byte[Math.max(2048,Math.min(16384,wanted/2))]; track.play();
        try{ int n; while(listening && (n=in.read(buf))>0){ int off=0; while(off<n){ int w=track.write(buf,off,n-off); if(w<=0) break; off+=w; } } }
        finally{ try{track.stop();}catch(Exception ignored){} track.release(); ui("Disconnected","Waiting for PC"); }
    }
    private void readFully(InputStream in, byte[] b) throws IOException { int o=0,n; while(o<b.length && (n=in.read(b,o,b.length-o))>0)o+=n; if(o!=b.length)throw new EOFException(); }
    private String localIp(){ try{ Enumeration<NetworkInterface> nis=NetworkInterface.getNetworkInterfaces(); while(nis.hasMoreElements()){ Enumeration<InetAddress> as=nis.nextElement().getInetAddresses(); while(as.hasMoreElements()){ InetAddress a=as.nextElement(); if(!a.isLoopbackAddress() && a instanceof Inet4Address) return a.getHostAddress(); }} }catch(Exception ignored){} return "unknown"; }
    @Override protected void onDestroy(){ stopServer(); if(wakeLock!=null && wakeLock.isHeld())wakeLock.release(); super.onDestroy(); }
}
