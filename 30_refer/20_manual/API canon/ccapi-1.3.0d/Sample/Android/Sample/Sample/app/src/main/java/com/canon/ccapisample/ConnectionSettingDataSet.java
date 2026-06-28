package com.canon.ccapisample;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.Serializable;

class ConnectionSettingDataSet implements Serializable {

    private String mSet;
    private String mNW1;
    private String mNW2;
    private String mMode1;
    private String mMode2;

    ConnectionSettingDataSet(String name, JSONObject object) {

        try {
            mSet = name;
            JSONArray array = object.getJSONArray("commsetting");
            mNW1 = array.getString(0);
            mNW2 = array.getString(1);
            array = object.getJSONArray("functionsetting");
            mMode1 = array.getString(0);
            mMode2 = array.getString(1);
        } catch (JSONException e) {
            e.printStackTrace();
        }

    }

    String getSet() {
        return mSet;
    }

    String getNW1() {
        return mNW1;
    }

    String getNW2() {
        return mNW2;
    }

    String getMode1() {
        return mMode1;
    }

    String getMode2() {
        return mMode2;
    }

}
