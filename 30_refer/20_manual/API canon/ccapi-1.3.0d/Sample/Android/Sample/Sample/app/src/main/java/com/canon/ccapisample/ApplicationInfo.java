package com.canon.ccapisample;

import android.app.Application;
import android.net.Uri;

public class ApplicationInfo extends Application {

    private Uri mSavePathUri;

    @Override
    public void onCreate() {
        super.onCreate();
        mSavePathUri = null;
    }

    @Override
    public void onTerminate() {
        super.onTerminate();
        mSavePathUri = null;
    }

    public void setSavePathUri(Uri uri) {
        mSavePathUri = uri;
    }

    public Uri getSavePathUri() {
        return mSavePathUri;
    }
}