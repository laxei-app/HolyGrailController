package com.canon.ccapisample;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

import static com.canon.ccapisample.Constants.CCAPI.Field.MESSAGE;
import static com.canon.ccapisample.Constants.CCAPI.Field.URL_LIST;

class WebAPIResultDataSet {
    private final Constants.RequestCode mRequestCode;
    private final String mRequestName;
    private final byte[] mRequestBody;
    private final String mUrl;
    private final String mQuery;
    private final String mMethod;
    private final HttpResultDataSet mHttpResultDataSet;

    WebAPIResultDataSet(Constants.RequestCode requestCode, String method, String url, byte[] requestBody, HttpResultDataSet httpResultDataSet){
        String[] parts = url.split("/");
        this.mRequestCode = requestCode;
        this.mRequestName = parts[parts.length-1];
        this.mUrl = url;
        this.mMethod = method;
        this.mRequestBody = requestBody;
        this.mHttpResultDataSet = httpResultDataSet;

        String query = null;
        try {
            query = new URL(mUrl).getQuery();
        }
        catch (MalformedURLException e) {
            e.printStackTrace();
        }
        finally {
            mQuery = query;
        }
    }

    int getResponseCode(){
        return mHttpResultDataSet.getResponseCode();
    }

    String getResponseBody() {
        return mHttpResultDataSet.getStringResponseBody();
    }

    Constants.RequestCode getRequestCode() {
        return mRequestCode;
    }

    String getRequestName() {
        return mRequestName;
    }

    String getMethod() {
        return mMethod;
    }

    String getUrl() {
        return mUrl;
    }

    Map<String, String> getQueryMap() {
        Map<String, String> map = null;
        if(mQuery != null) {
            map = new HashMap<>();
            String[] queries = mQuery.split("&");
            for (String query : queries) {
                String[] split = query.split("=");
                map.put(split[0], split[1]);
            }
        }
        return map;
    }

    String getRequestBodyFromKey(String key){
        String result = null;
        if(mRequestBody != null) {
            try {
                JSONObject json = new JSONObject(new String(mRequestBody, StandardCharsets.UTF_8));
                result = json.get(key).toString();
            }
            catch (JSONException e) {
                e.printStackTrace();
            }
        }
        return result;
    }

    String getResponseBodyFromKey(String key){
        String result = null;
        if(mHttpResultDataSet.getStringResponseBody() != null) {
            try {
                JSONObject json = new JSONObject(mHttpResultDataSet.getStringResponseBody());
                result = json.get(key).toString();
            }
            catch (JSONException e) {
                e.printStackTrace();
            }
        }
        return result;
    }

    byte[] getBytesResponseBody(){
        return mHttpResultDataSet.getBytesResponseBody();
    }

    Bitmap getImageResponseBody(){
        Bitmap bitmap = null;
        byte[] bytes = mHttpResultDataSet.getBytesResponseBody();
        //Do not read if content type is "image/heif".
        String contenttype = mHttpResultDataSet.getResponseHeaderMap().get("Content-Type");
        if(bytes != null && !contenttype.equals("image/heif")){
            BitmapFactory.Options options = new  BitmapFactory.Options();
            options.inMutable = true;
            bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
        }
        return bitmap;
    }

    boolean isError() {
        boolean isError = false;
        if(mHttpResultDataSet != null) {
            if ((mHttpResultDataSet.getResponseCode() != HttpURLConnection.HTTP_OK && mHttpResultDataSet.getResponseCode() != HttpURLConnection.HTTP_ACCEPTED) || mHttpResultDataSet.getCancel()) {
                isError = true;
            }
        }
        else{
            isError = true;
        }
        return isError;
    }

    boolean isCancel(){
        boolean isCancel = false;
        if(mHttpResultDataSet.getCancel()){
            isCancel = true;
        }
        return isCancel;
    }

    String getErrorMsg(){
        StringBuilder stringBuilder = new StringBuilder();
        stringBuilder.append(mRequestName).append(" : ");

        if(mHttpResultDataSet.getResponseCode() != 0){
            stringBuilder.append(mHttpResultDataSet.getResponseCode()).append(" ");
        }

        if (mHttpResultDataSet.getCancel()){
            stringBuilder.append("Canceled");
        }
        else if (!mHttpResultDataSet.getStringResponseBody().isEmpty()) {
            try {
                JSONObject jsonObject = new JSONObject(mHttpResultDataSet.getStringResponseBody());
                if (!jsonObject.isNull(MESSAGE)) {
                    stringBuilder.append(jsonObject.getString(MESSAGE));
                }

                if (!jsonObject.isNull(URL_LIST)) {
                    stringBuilder.append('\n');
                    JSONArray jsonArray = jsonObject.getJSONArray(URL_LIST);
                    for(int i = 0; i < jsonArray.length(); i++){
                        stringBuilder.append(jsonArray.getString(i));
                        if( i + 1 < jsonArray.length()){
                            stringBuilder.append('\n');
                        }
                    }
                }
            }
            catch (JSONException e) {
                e.printStackTrace();
            }
        }
        else {
            stringBuilder.append("Error");
        }
        return stringBuilder.toString();
    }
}
