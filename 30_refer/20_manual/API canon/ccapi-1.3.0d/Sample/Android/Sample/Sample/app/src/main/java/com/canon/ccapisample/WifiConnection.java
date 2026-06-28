package com.canon.ccapisample;

import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.net.DatagramPacket;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

class WifiConnection {
    private static String SSDP_ADDR;
    private static final String TAG = WifiConnection.class.getSimpleName();
    private static final String NETWORK_IF_NAME = "wlan0";  // It depends on the Android model. If it does not work, rewrite it according to the target.
    private static final int RETRY_COUNT = 3;
    private static final int SSDP_RECEIVE_TIMEOUT = 5000;
    private static final int PACKET_BUFFER_SIZE = 1024;
    private static final int SSDP_PORT = 1900;
    private static final int SSDP_MX = 1;
    private static final String SSDP_ADDR_V4 = "239.255.255.250";
    private static final String SSDP_ADDR_V6 = "[FF02::C]";
    private static final String SSDP_ST = "urn:schemas-canon-com:service:ICPO-CameraControlAPIService:1";
    private static final String DD_REGEX = "<ns:X_accessURL xmlns:ns=\"urn:schemas-canon-com:schema-upnp\">"
                                                + "(.+?)"
                                                + "</ns:X_accessURL>";

    WifiConnection(boolean isUseIPv6){
        SSDP_ADDR = isUseIPv6 ? SSDP_ADDR_V6 : SSDP_ADDR_V4;
    }

    String execute(){
        return discovery();
    }

    private String discovery(){
        String url = null;
        String replyMessage;

        Log.d(TAG, "Discovery.");

        replyMessage = sendMSearch();

        if(replyMessage != null){
            String ddLocation = findParameterValue(replyMessage, "Location");
            Log.d(TAG, ddLocation);

            if(ddLocation != null){
                url = getBaseUrl(ddLocation);
            }
        }

        return url;
    }

    private String sendMSearch(){
        String replyMessage = null;
        int retryCount = 0;

        final String ssdpRequest = "M-SEARCH * HTTP/1.1\r\n" +
                "HOST:" + SSDP_ADDR + ":" + SSDP_PORT + "\r\n" +
                "MAN: \"ssdp:discover\"\r\n" +
                "MX: " + SSDP_MX +"\r\n" +
                "ST: " + SSDP_ST + "\r\n\r\n";
        final byte[] sendData = ssdpRequest.getBytes();

        while(retryCount < RETRY_COUNT) {
            MulticastSocket socket = null;
            DatagramPacket receivePacket;
            DatagramPacket packet;

            if(retryCount != 0){
                try {
                    Thread.sleep(1000);
                }
                catch (InterruptedException e) {
                    e.printStackTrace();
                }
            }

            try {
                Log.d(TAG, "M-SEARCH Send.");
                NetworkInterface ifc = NetworkInterface.getByName(NETWORK_IF_NAME);
                socket = new MulticastSocket();
                socket.setNetworkInterface(ifc);
                InetSocketAddress iAddress = new InetSocketAddress(SSDP_ADDR, SSDP_PORT);
                packet = new DatagramPacket(sendData, sendData.length, iAddress);
                socket.send(packet);
            }
            catch (IOException e) {
                e.printStackTrace();

                if (socket != null) {
                    socket.close();
                }
                retryCount++;
                continue;
            }

            byte[] array = new byte[PACKET_BUFFER_SIZE];

            Log.d(TAG, "Waiting.");

            receivePacket = new DatagramPacket(array, array.length);
            try {
                socket.setSoTimeout(SSDP_RECEIVE_TIMEOUT);
                socket.receive(receivePacket);
                replyMessage = new String(receivePacket.getData(), 0, receivePacket.getLength(), StandardCharsets.UTF_8);

                Log.d(TAG, "Receive.");
                Log.d(TAG, replyMessage);
                break;
            }
            catch (IOException e) {
                e.printStackTrace();
                retryCount++;
            }
            finally {
                socket.close();
            }
        }

        return replyMessage;
    }

    private String findParameterValue(String message, String paramName) {
        String name = paramName;
        if (!name.endsWith(":")) {
            name = name + ":";
        }
        int start = message.indexOf(name);
        int end = message.indexOf("\r\n", start);
        if (start != -1 && end != -1) {
            start += name.length();
            String val = message.substring(start, end);
            return val.trim();
        }
        return null;
    }

    private String getBaseUrl(String ddLocation){
        String ret = null;
        String xml = "";
        InputStream inputStream = null;

        try {
            URL url = new URL(ddLocation);
            byte[] buf = new byte[PACKET_BUFFER_SIZE];
            inputStream = url.openStream();
            while (inputStream.read(buf) != -1) {
                String read = new String(buf, StandardCharsets.UTF_8);
                xml += read;
                Arrays.fill(buf,(byte)0);
            }
            Log.d(TAG, xml);
        }
        catch (IOException e) {
            e.printStackTrace();
        }
        finally {
            try {
                if (inputStream != null) {
                    inputStream.close();
                }
            }
            catch (IOException e) {
                e.printStackTrace();
            }
        }

        Pattern p = Pattern.compile(DD_REGEX);
        Matcher m = p.matcher(xml);
        if (m.find()) {
            do {
                ret = m.group(1);
                Log.d(TAG, m.group(1));
            } while (m.find());
        }

        return ret;
    }
}
