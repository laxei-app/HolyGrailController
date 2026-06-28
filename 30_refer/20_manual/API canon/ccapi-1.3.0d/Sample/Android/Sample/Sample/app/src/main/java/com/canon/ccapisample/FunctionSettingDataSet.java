package com.canon.ccapisample;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.Serializable;

class FunctionSettingDataSet implements Serializable {

    private String mMode;
    private String mCommfunction;

    FunctionSettingDataSet(String name, JSONObject object) {
        try {
            mMode = name;
            mCommfunction = object.getString("commfunction");
        } catch (JSONException e) {
            e.printStackTrace();
        }
    }

    String getMode() {
        return mMode;
    }

    String getCommfunction() {
        return mCommfunction;
    }

}
