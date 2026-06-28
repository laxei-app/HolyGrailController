package com.canon.ccapisample;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.Serializable;

class CommSettingDataSet implements Serializable {

    private String mNW;
    private String mLantype;
    private String mIpv4_ipaddressset;
    private String mIpv4_ipaddress;
    private String mIpv4_subnetmask;
    private String mIpv4_gateway;
    private String mIpv6_useipv6;
    private String mIpv6_manual_setting;
    private String mIpv6_manual_address;
    private String mIpv6_prefixlength;
    private String mIpv6_gateway;
    private String mSsid;
    private String mMethod;
    private String mChannel;
    private String mAuthentication;
    private String mEncryption;
    private String mKeyindex;
    private String mPassword;

    CommSettingDataSet(String name, JSONObject object) {

        try {
            mNW = name;
            mLantype = object.getString("lantype");
            mIpv4_ipaddressset = object.getString("ipv4_ipaddressset");
            mIpv4_ipaddress = object.getString("ipv4_ipaddress");
            mIpv4_subnetmask = object.getString("ipv4_subnetmask");
            mIpv4_gateway = object.getString("ipv4_gateway");
            mIpv6_useipv6 = object.getString("ipv6_useipv6");
            mIpv6_manual_setting = object.getString("ipv6_manual_setting");
            mIpv6_manual_address = object.getString("ipv6_manual_address");
            mIpv6_prefixlength = object.getString("ipv6_prefixlength");
            mIpv6_gateway = object.getString("ipv6_gateway");
            mSsid = object.getString("ssid");
            mMethod = object.getString("method");
            mChannel = object.getString("channel");
            mAuthentication = object.getString("authentication");
            mEncryption = object.getString("encryption");
            mKeyindex = object.getString("keyindex");
            mPassword = object.getString("password");
        } catch (JSONException e) {
            e.printStackTrace();
        }
    }

    String getNW() {
        return mNW;
    }

    String getLantype() {
        return mLantype;
    }

    String getIpv4_ipaddressset() { return mIpv4_ipaddressset; }

    String getIpv4_ipaddress() {
        return mIpv4_ipaddress;
    }

    String getIpv4_subnetmask() {
        return mIpv4_subnetmask;
    }

    String getIpv4_gateway() {
        return mIpv4_gateway;
    }

    String getIpv6_useipv6() {
        return mIpv6_useipv6;
    }

    String getIpv6_manual_setting() {
        return mIpv6_manual_setting;
    }

    String getIpv6_manual_address() {
        return mIpv6_manual_address;
    }

    String getIpv6_prefixlength() {
        return mIpv6_prefixlength;
    }

    String getIpv6_gateway() {
        return mIpv6_gateway;
    }

    String getSsid() {
        return mSsid;
    }

    String getMethod() {
        return mMethod;
    }

    String getChannel() {
        return mChannel;
    }

    String getAuthentication() {
        return mAuthentication;
    }

    String getEncryption() {
        return mEncryption;
    }

    String getKeyindex() {
        return mKeyindex;
    }

    String getPassword() {
        return mPassword;
    }
}
