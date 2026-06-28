package com.canon.ccapisample;

import java.nio.charset.StandardCharsets;
import java.util.Map;

class HttpResultDataSet {
    private final int mResponseCode;
    private final String mResponseMessage;
    private final Map<String, String> mResponseHeaderMap;
    private final byte[] mBytesResponseBody;
    private final boolean mIsCancel;

    HttpResultDataSet(int responseCode, String responseMessage, Map<String, String> responseHeaderMap, byte[] bytesResponseBody, Boolean isCancel){
        this.mResponseCode = responseCode;
        this.mResponseMessage = responseMessage;
        this.mResponseHeaderMap = responseHeaderMap;
        this.mBytesResponseBody = bytesResponseBody;
        this.mIsCancel = isCancel;
    }

    HttpResultDataSet(int responseCode, String responseMessage, Map<String, String> responseHeaderMap, byte[] bytesResponseBody){
        this.mResponseCode = responseCode;
        this.mResponseMessage = responseMessage;
        this.mResponseHeaderMap = responseHeaderMap;
        this.mBytesResponseBody = bytesResponseBody;
        this.mIsCancel = false;
    }

    int getResponseCode() {
        return mResponseCode;
    }

    String getStringResponseBody() {
        String body = "";
        if(mBytesResponseBody != null) {
            try {
                body = new String(mBytesResponseBody, StandardCharsets.UTF_8);
            }
            catch (OutOfMemoryError e) {
                e.printStackTrace();
            }
        }
        return body;
    }

    byte[] getBytesResponseBody() {
        return mBytesResponseBody;
    }

    Map<String, String> getResponseHeaderMap() {
        return mResponseHeaderMap;
    }

    boolean getCancel() {
        return mIsCancel;
    }
}
