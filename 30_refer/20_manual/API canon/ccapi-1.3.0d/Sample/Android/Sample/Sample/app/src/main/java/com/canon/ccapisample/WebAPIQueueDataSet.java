package com.canon.ccapisample;

import android.os.Bundle;

public class WebAPIQueueDataSet {
    private final WebAPIResultListener mWebAPIResultListener;
    private final Constants.RequestCode mRequestCode;
    private final Bundle mArguments;

    private HttpProgressListener mHttpProgressListener = null;
    private boolean mIsParallel = false;
    private ChunkResultListener mChunkResultListener = null;

    WebAPIQueueDataSet(Constants.RequestCode requestCode, Bundle args, WebAPIResultListener webAPIResultListener){
        this.mWebAPIResultListener = webAPIResultListener;
        this.mRequestCode = requestCode;
        this.mArguments = args;
    }

    WebAPIQueueDataSet(Constants.RequestCode requestCode, Bundle args, WebAPIResultListener webAPIResultListener, HttpProgressListener httpProgressListener){
        this.mWebAPIResultListener = webAPIResultListener;
        this.mRequestCode = requestCode;
        this.mArguments = args;
        this.mHttpProgressListener = httpProgressListener;
    }

    WebAPIQueueDataSet(Constants.RequestCode requestCode, Bundle args, boolean isParallel, WebAPIResultListener webAPIResultListener){
        this.mWebAPIResultListener = webAPIResultListener;
        this.mRequestCode = requestCode;
        this.mArguments = args;
        this.mIsParallel = isParallel;
    }

    WebAPIQueueDataSet(Constants.RequestCode requestCode, Bundle args, boolean isParallel, ChunkResultListener chunkResultListener, WebAPIResultListener webAPIResultListener){
        this.mWebAPIResultListener = webAPIResultListener;
        this.mRequestCode = requestCode;
        this.mArguments = args;
        this.mIsParallel = isParallel;
        this.mChunkResultListener = chunkResultListener;
    }

    WebAPIResultListener getWebAPIResultListener() {
        return mWebAPIResultListener;
    }

    Constants.RequestCode getRequestCode() {
        return mRequestCode;
    }

    Bundle getArguments() {
        return mArguments;
    }

    boolean isParallel() {
        return mIsParallel;
    }

    ChunkResultListener getChunkResultListener() {
        return mChunkResultListener;
    }

    HttpProgressListener getHttpProgressListener() {
        return mHttpProgressListener;
    }
}
