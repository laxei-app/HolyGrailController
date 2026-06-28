package com.canon.ccapisample;

import org.json.JSONException;
import org.json.JSONObject;

class APIDataSet {
    private final String mUrl;
    private final String mKey;
    private final String mVersion;
    private final boolean mGetable;
    private final boolean mPostable;
    private final boolean mPutable;
    private final boolean mDeletable;

    APIDataSet(String baseUrl, String version, JSONObject api){
        String url = "";
        String key = "";
        boolean getable = false;
        boolean postable = false;
        boolean putable = false;
        boolean deletable = false;

        try {
            if(api.has("path")){
                String path = api.getString("path");
                url = baseUrl + path.replace("/ccapi","");
            }else{
                url = api.getString("url");
            }
            key = url.replace(baseUrl + "/" + version + "/", "");
            getable = api.getBoolean("get");
            postable = api.getBoolean("post");
            putable = api.getBoolean("put");
            deletable = api.getBoolean("delete");
        }
        catch (JSONException e) {
            e.printStackTrace();
        }
        finally {
            this.mUrl = url;
            this.mKey = key;
            this.mVersion = version;
            this.mGetable = getable;
            this.mPostable = postable;
            this.mPutable = putable;
            this.mDeletable = deletable;
        }
    }

    public String getUrl() {
        return mUrl;
    }

    public String getKey() {
        return mKey;
    }

    public String getVersion() { return mVersion; }

    public boolean isGetable() {
        return mGetable;
    }

    public boolean isPostable() {
        return mPostable;
    }

    public boolean isPutable() {
        return mPutable;
    }

    public boolean isDeletable() {
        return mDeletable;
    }
}
