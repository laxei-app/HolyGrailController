package com.canon.ccapisample;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;

import android.os.Handler;
import android.util.Log;
import android.widget.ImageView;
import android.widget.TextView;

import org.json.JSONException;
import org.json.JSONObject;

import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import static com.canon.ccapisample.Constants.CCAPI.Field.AF_AREA;
import static com.canon.ccapisample.Constants.CCAPI.Field.AF_FRAME;
import static com.canon.ccapisample.Constants.CCAPI.Field.AF_MODE_AUTO;
import static com.canon.ccapisample.Constants.CCAPI.Field.AF_MODE_LARGE_ZONE;
import static com.canon.ccapisample.Constants.CCAPI.Field.AF_MODE_ZONE;
import static com.canon.ccapisample.Constants.CCAPI.Field.HEIGHT;
import static com.canon.ccapisample.Constants.CCAPI.Field.SELECT;
import static com.canon.ccapisample.Constants.CCAPI.Field.VALUE;
import static com.canon.ccapisample.Constants.CCAPI.Field.VISIBLE;
import static com.canon.ccapisample.Constants.CCAPI.Field.WIDTH;
import static com.canon.ccapisample.Constants.CCAPI.Field.X;
import static com.canon.ccapisample.Constants.CCAPI.Field.Y;
import static com.canon.ccapisample.Constants.CCAPI.Value.NOT_SELECTABLE;
import static com.canon.ccapisample.Constants.CCAPI.Value.NOT_SELECTED;
import static com.canon.ccapisample.Constants.CCAPI.Value.SELECTED;
import static com.canon.ccapisample.Constants.RemoteCapture.FINEDER_HEIGHT;
import static com.canon.ccapisample.Constants.RemoteCapture.FINEDER_WIDTH;

class OpticalFinderThread extends Thread{
    interface Callback{
        void onComplete();
    }

    private static final String TAG = OpticalFinderThread.class.getSimpleName();

    private final WeakReference<ImageView> mImageViewReference;
    final WeakReference<TextView> mAfAreaSelectionModeTextViewReference;
    final WeakReference<TextView> mAfAreaSelectionIdTextViewReference;
    private final Handler mHandler;
    private final WebAPI mWebAPI;
    private Bitmap mBitmap;

    private JSONObject mAfFrameInfoObject = null;
    private JSONObject mAfAreaInfoObject = null;

    OpticalFinderThread(ImageView imageView, TextView AfAreaSelectionModeText, TextView AfAreaSelectionIdText){
        this.mHandler = new Handler();
        this.mImageViewReference = new WeakReference<>(imageView);
        this.mAfAreaSelectionModeTextViewReference = new WeakReference<>(AfAreaSelectionModeText);
        this.mAfAreaSelectionIdTextViewReference = new WeakReference<>(AfAreaSelectionIdText);
        this.mWebAPI = WebAPI.getInstance();
    }

    @Override
    public void run() {

        Log.d(TAG, "OpticalFinder Start");
        OpticalFinder();

    }

    void stopThread(final Callback callback){

        if(isAlive()) {
            interrupt();
            callback.onComplete();
        }
        else{
            callback.onComplete();
        }
    }

    private void notifyThread(){
        synchronized (this) {
            this.notifyAll();
        }
    }

    private void OpticalFinder(){

        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_SHOOTING_OPTICALFINDER_AFAREASELECTION, null, false, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "afareaselection onWebAPIResult begin.");
                if (result.isError()) {
                    Log.d(TAG, "afareaselection onWebAPIResult Error.");
                }else{
                    try {
                        JSONObject obj = new JSONObject(result.getResponseBody());
                        String ID = obj.getString(VALUE);
                        final TextView AfAreaSelectionModeText = mAfAreaSelectionIdTextViewReference.get();
                        if (AfAreaSelectionModeText != null) {
                            AfAreaSelectionModeText.setText(ID);
                        }
                    } catch (JSONException e) {
                        e.printStackTrace();
                    }
                }
                notifyThread();
                Log.d(TAG, "afareaselection onWebAPIResult end.");
            }
        }));

        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_SHOOTING_OPTICALFINDER_AFFRAMEINFORMATION, null, false, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "afpointinformation onWebAPIResult begin.");
                if (result.isError()) {
                    Log.d(TAG, "afpointinformation onWebAPIResult Error.");
                }else{
                    try {
                        mAfFrameInfoObject = new JSONObject(result.getResponseBody());
                    } catch (JSONException e) {
                        e.printStackTrace();
                    }
                }
                notifyThread();
                Log.d(TAG, "afpointinformation onWebAPIResult end.");
            }
        }));

        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_SHOOTING_OPTICALFINDER_AFAREAINFORMATION, null, false, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "afareainformation onWebAPIResult begin.");
                if (result.isError()) {
                    Log.d(TAG, "afareainformation onWebAPIResult Error.");
                }else{
                    try {
                        mAfAreaInfoObject = new JSONObject(result.getResponseBody());
                    } catch (JSONException e) {
                        e.printStackTrace();
                    }
                }
                notifyThread();
                Log.d(TAG, "afareainformation onWebAPIResult end.");
            }
        }));

        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_SHOOTING_OPTICALFINDER_AFAREASELECTIONMODE, null, false, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Log.d(TAG, "afareaselectionmode onWebAPIResult begin.");
                if (result.isError()) {
                    Log.d(TAG, "afareaselectionmode onWebAPIResult Error.");
                }else{
                    try {
                        JSONObject obj = new JSONObject(result.getResponseBody());
                        String mode = obj.getString(VALUE);
                        Bitmap bitmap = Bitmap.createBitmap(FINEDER_WIDTH, FINEDER_HEIGHT, Bitmap.Config.ARGB_8888);
                        if (bitmap != null) {
                            updateOpticalFinder(bitmap, mode);
                        }
                        final TextView AfAreaSelectionModeText = mAfAreaSelectionModeTextViewReference.get();
                        if (AfAreaSelectionModeText != null) {
                            AfAreaSelectionModeText.setText(mode);
                        }
                    } catch (JSONException e) {
                        e.printStackTrace();
                    }
                }
                notifyThread();
                Log.d(TAG, "afareaselectionmode onWebAPIResult end.");
            }
        }));
    }

    private void updateOpticalFinder(Bitmap bitmap, String mode){

        final ImageView imageView = mImageViewReference.get();
        if (imageView != null) {
            final Bitmap OpticalFinderBitmap = createOpticalFinderBitmap(bitmap, mode);

            mHandler.post(new Runnable() {
                public void run() {
                    if (mBitmap != null) {
                        imageView.setImageDrawable(null);
                        imageView.setImageBitmap(null);
                        mBitmap.recycle();
                        mBitmap = null;
                    }
                    mBitmap = OpticalFinderBitmap;
                    imageView.setImageBitmap(mBitmap);
                    Log.d(TAG, "updateOpticalFineder setImageBitmap");
                }
            });
        }
        else {
            interrupt();
            Log.d(TAG, "updateOpticalFineder ImageView not enabled.");
        }
    }

    private Bitmap createOpticalFinderBitmap(Bitmap bitmap, String mode){

        createFinederArea(bitmap);
        createAfFrame(bitmap, mode);

        return bitmap;
    }

    private void createFinederArea(Bitmap bitmap){

        Paint paint = new Paint();
        Canvas canvas = new Canvas(bitmap);
        paint.setStyle(Paint.Style.FILL_AND_STROKE);
        paint.setColor(Color.BLACK);

        canvas.drawRect(0, 0, FINEDER_WIDTH, FINEDER_HEIGHT, paint);
    }

    private void createAfFrame(Bitmap bitmap, String mode){
        Paint paint = new Paint();
        Canvas canvas = new Canvas(bitmap);
        int mainColor = 0;
        paint.setStrokeWidth(2);
        paint.setStyle(Paint.Style.STROKE);

        /* Do not generate when in "auto" mode */
        if(!mode.equals(AF_MODE_AUTO)) {

            try {
                /* Draw AF frame */
                JSONObject mAfFrameObject = mAfFrameInfoObject.getJSONObject(AF_FRAME);

                int visible_x = getImagePositionX();
                int visible_y = getImagePositionY();
                int visible_w = getImagePositionWidth();
                int visible_h = getImagePositionHeight();

                List<JSONObject> afFrameList = new ArrayList<>();
                if (mAfFrameObject.length() != 0) {
                    Iterator<String> iterator = mAfFrameObject.keys();
                    while (iterator.hasNext()) {
                        String key = iterator.next();
                        JSONObject frame = mAfFrameObject.getJSONObject(key);
                        afFrameList.add(frame);
                    }
                }

                Iterator<String> iterator_frame = mAfFrameObject.keys();

                while (iterator_frame.hasNext()) {

                    String key = iterator_frame.next();
                    JSONObject frame = mAfFrameObject.getJSONObject(key);

                    int select = frame.getInt(SELECT);

                    if (select != NOT_SELECTABLE) {

                        if (select == NOT_SELECTED) {
                            mainColor = Color.WHITE;
                        } else if (select == SELECTED) {
                            mainColor = Color.RED;
                        }

                        float widthScale = visible_w / (float) bitmap.getWidth();
                        float heightScale = visible_h / (float) bitmap.getHeight();
                        int x = Math.round((float) (frame.getInt(X) - visible_x) / widthScale);
                        int y = Math.round((float) (frame.getInt(Y) - visible_y) / heightScale);
                        int width = Math.round((float) frame.getInt(WIDTH) / widthScale);
                        int height = Math.round((float) frame.getInt(HEIGHT) / heightScale);

                        paint.setColor(mainColor);
                        canvas.drawRect(x, y, x + width, y + height, paint);
                    }
                }

                /* Drawing the AF area */
                JSONObject mAfAreaObject = mAfAreaInfoObject.getJSONObject(mode).getJSONObject(AF_AREA);

                List<JSONObject> afAreaList = new ArrayList<>();
                if (mAfAreaObject.length() != 0) {
                    Iterator<String> iterator = mAfAreaObject.keys();
                    while (iterator.hasNext()) {
                        String key = iterator.next();
                        JSONObject frame = mAfAreaObject.getJSONObject(key);
                        afAreaList.add(frame);
                    }
                }

                Iterator<String> iterator_area = mAfAreaObject.keys();

                while (iterator_area.hasNext()) {

                    String key = iterator_area.next();
                    JSONObject area = mAfAreaObject.getJSONObject(key);

                    int select = area.getInt(SELECT);

                    if (select != NOT_SELECTABLE) {

                        float widthScale = visible_w / (float) bitmap.getWidth();
                        float heightScale = visible_h / (float) bitmap.getHeight();
                        int x = Math.round((float) (area.getInt(X) - visible_x) / widthScale);
                        int y = Math.round((float) (area.getInt(Y) - visible_y) / heightScale);
                        int width = Math.round((float) area.getInt(WIDTH) / widthScale);
                        int height = Math.round((float) area.getInt(HEIGHT) / heightScale);

                        if (select == NOT_SELECTED) {
                            mainColor = Color.WHITE;
                            paint.setColor(mainColor);
                            canvas.drawRect(x, y, x + width, y + height, paint);
                        } else if (select == SELECTED) {
                            mainColor = Color.RED;
                            paint.setColor(mainColor);
                            canvas.drawRect(x - 2, y - 2, x + width + 4, y + height + 4, paint);
                        }
                    }
                }

            } catch (JSONException e) {
                e.printStackTrace();
            }

        }
    }

    public String getAfFrameID(Integer x, Integer y, String mode){

        Log.d(TAG, "mode :" + mode);
        String ID = null;

        try {

            /* Do not set in "auto" mode */
            if(!mode.equals(AF_MODE_AUTO)){

                List<String>  IdList = new ArrayList<>();
                List<Integer>  X_CoordinateList = new ArrayList<>();
                List<Integer>  Y_CoordinateList = new ArrayList<>();

                /* In Zone Mode */
                if (mode.equals(AF_MODE_ZONE) || mode.equals(AF_MODE_LARGE_ZONE)) {
                    JSONObject mAfAreaObject = mAfAreaInfoObject.getJSONObject(mode).getJSONObject(AF_AREA);
                    if (mAfAreaObject.length() != 0) {
                        Iterator<String> iterator = mAfAreaObject.keys();
                        while (iterator.hasNext()) {
                            String key = iterator.next();
                            JSONObject area = mAfAreaObject.getJSONObject(key);
                            int select = area.getInt(SELECT);
                            if(select != NOT_SELECTABLE){
                                IdList.add(key);
                                /* Calculate the coordinates of the center of a point */
                                X_CoordinateList.add(area.getInt(X) + area.getInt(WIDTH) / 2);
                                Y_CoordinateList.add(area.getInt(Y) + area.getInt(HEIGHT) / 2);
                            }
                        }
                    }
                } else {
                    JSONObject mAfFrameObject = mAfFrameInfoObject.getJSONObject(AF_FRAME);
                    if (mAfFrameObject.length() != 0) {
                        Iterator<String> iterator = mAfFrameObject.keys();
                        while (iterator.hasNext()) {
                            String key = iterator.next();
                            JSONObject frame = mAfFrameObject.getJSONObject(key);
                            int select = frame.getInt(SELECT);
                            if(select != NOT_SELECTABLE) {
                                IdList.add(key);
                                /* Calculate the coordinates of the center of a point */
                                X_CoordinateList.add(frame.getInt(X) + frame.getInt(WIDTH) / 2);
                                Y_CoordinateList.add(frame.getInt(Y) + frame.getInt(HEIGHT) / 2);
                            }
                        }
                    }
                }

                /* Find the closest point to the touched coordinates */
                double min = Math.sqrt(Math.pow((X_CoordinateList.get(0) - x), 2) + Math.pow((Y_CoordinateList.get(0) - y), 2));
                for (int i = 0; i < IdList.size(); i++) {
                    if (Math.sqrt(Math.pow((X_CoordinateList.get(i) - x), 2) + Math.pow((Y_CoordinateList.get(i) - y), 2)) <=  min) {
                        ID = IdList.get(i);
                        min = Math.sqrt(Math.pow((X_CoordinateList.get(i) - x), 2) + Math.pow((Y_CoordinateList.get(i) - y), 2));
                    }
                }
            }

        } catch (JSONException e) {
            e.printStackTrace();
        }

        Log.d(TAG, "Selected ID: " + ID);
        return ID;
    }

    int getImagePositionX(){
        int x = 0;
        JSONObject mAfVisibleObject;
        try {
            mAfVisibleObject = mAfFrameInfoObject.getJSONObject(VISIBLE);
            x = mAfVisibleObject.getInt(X);
        } catch (JSONException e) {
            e.printStackTrace();
        }
        return x;
    }

    int getImagePositionY(){
        int y = 0;
        JSONObject mAfVisibleObject;
        try {
            mAfVisibleObject = mAfFrameInfoObject.getJSONObject(VISIBLE);
            y = mAfVisibleObject.getInt(Y);
        } catch (JSONException e) {
            e.printStackTrace();
        }
        return y;
    }

    int getImagePositionWidth(){
        int w = 0;
        JSONObject mAfVisibleObject;
        try {
            mAfVisibleObject = mAfFrameInfoObject.getJSONObject(VISIBLE);
            w = mAfVisibleObject.getInt(WIDTH);
        } catch (JSONException e) {
            e.printStackTrace();
        }
        return w;
    }

    int getImagePositionHeight(){
        int h = 0;
        JSONObject mAfVisibleObject;
        try {
            mAfVisibleObject = mAfFrameInfoObject.getJSONObject(VISIBLE);
            h = mAfVisibleObject.getInt(HEIGHT);
        } catch (JSONException e) {
            e.printStackTrace();
        }
        return h;
    }
}
