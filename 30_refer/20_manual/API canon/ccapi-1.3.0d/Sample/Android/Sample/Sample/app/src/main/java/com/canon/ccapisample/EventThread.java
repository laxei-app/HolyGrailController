package com.canon.ccapisample;

import android.os.Bundle;
import android.os.Handler;
import android.util.Log;

import java.nio.charset.StandardCharsets;

class EventThread extends Thread implements ChunkResultListener{
    interface Callback{
        void onComplete();
    }

    private static final String TAG = EventThread.class.getSimpleName();
    private final Handler mHandler;
    private final Constants.EventMethod mMethod;
    private final EventListener mEventListener;
    private final WebAPI mWebAPI;

    EventThread(Constants.EventMethod method, EventListener eventListener){
        this.mHandler = new Handler();
        this.mMethod = method;
        this.mEventListener = eventListener;
        this.mWebAPI = WebAPI.getInstance();
    }

    @Override
    public void run() {
        Log.d(TAG, "EventThread Begin. " + mMethod.toString());

        while(!isInterrupted()) {
            try {
                synchronized (this) {
                    switch(mMethod){
                        case MONITORING:
                            requestGetEventMonitoring();
                            break;
                        case POLLING_CONTINUE:
                            requestGetEventPolling("continue=on");
                            break;
                        case POLLING_SHORT:
                            requestGetEventPolling("timeout=short");
                            break;
                        case POLLING_LONG:
                            requestGetEventPolling("timeout=long");
                            break;
                        case POLLING_IMMEDIATELY:
                            requestGetEventPolling("timeout=immediately");
                            break;
                        case POLLING:
                        default:
                            requestGetEventPolling(null);
                            break;
                    }

                    this.wait();
                }
            }
            catch (InterruptedException e) {
                e.printStackTrace();
                break;
            }
        }

        Log.d(TAG, "EventThread End. " + mMethod.toString());
    }

    void stopThread(final Callback callback){
        if(isAlive()) {
            WebAPIResultListener webAPIResultListener = new WebAPIResultListener() {
                @Override
                public void onWebAPIResult(WebAPIResultDataSet result) {
                    interrupt();
                    Log.d(TAG, String.format("stopThread : onWebAPIResult. %s", mMethod.toString()));
                    callback.onComplete();
                }
            };

            switch (mMethod) {
                case POLLING:
                case POLLING_IMMEDIATELY:
                    interrupt();
                    callback.onComplete();
                    break;
                case POLLING_SHORT:
                case POLLING_LONG:
                case POLLING_CONTINUE:
                    interrupt();
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.DELETE_EVENT_POLLING, null, webAPIResultListener));
                    break;
                case MONITORING:
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.DELETE_EVENT_MONITORING, null, webAPIResultListener));
                    break;
                default:
                    break;
            }
        }
        else{
            callback.onComplete();
        }
        Log.d(TAG, "EventThread stopThread() " + mMethod.toString());
    }

    private void notifyThread(){
        synchronized (this) {
            this.notifyAll();
        }
    }

    private void requestGetEventPolling(String Query){
        Log.d(TAG, "getEventPolling begin.");
        Bundle args = new Bundle();
        args.putString(Constants.RequestCode.GET_EVENT_POLLING.name(), Query);
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_EVENT_POLLING, args, true, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "getEventPolling onWebAPIResult begin.");
                if(!result.isError()){
                    Log.d(TAG, "getEventPolling onWebAPIResult : " + result.getResponseBody());
                    mEventListener.onNotifyEvent(result.getResponseBody());
                }
                notifyThread();
                Log.d(TAG, "getEventPolling onWebAPIResult end.");
            }
        }));
        Log.d(TAG, "getEventPolling end.");
    }

    /**
     * Callback from execute WebAPI chunked format
     * @param bytes bytes
     * @return boolean
     */
    @Override
    public boolean onChunkResult(byte[] bytes) {
        Log.d(TAG, "getEventMonitoring onChunkResult begin.");
        OrgFormatDataSet orgFormatDataSet = new OrgFormatDataSet(bytes);
        if(orgFormatDataSet.getEventData() != null) {
            byte[] eventData = orgFormatDataSet.getEventData();
            if(eventData != null){
                final String response = new String(eventData, StandardCharsets.UTF_8);
                mHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        mEventListener.onNotifyEvent(response);
                    }
                });
            }
        }
        Log.d(TAG, "getEventMonitoring onChunkResult end.");
        return isAlive();
    }

    private void requestGetEventMonitoring(){
        Log.d(TAG, "getEventMonitoring begin.");
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_EVENT_MONITORING, null, true, this, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "getEventMonitoring onWebAPIResult.");
                if (result.isError()) {
                    Log.d(TAG, "getEventMonitoring onWebAPIResult Error.");
                }
                notifyThread();
            }
        }));
        Log.d(TAG, "getEventMonitoring end.");
    }
}
