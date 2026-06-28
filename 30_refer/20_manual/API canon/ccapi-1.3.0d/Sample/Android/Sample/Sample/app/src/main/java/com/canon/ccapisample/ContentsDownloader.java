package com.canon.ccapisample;

import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.support.v4.app.FragmentActivity;
import android.support.v4.app.FragmentTransaction;
import android.support.v4.provider.DocumentFile;
import android.util.Log;
import android.webkit.MimeTypeMap;
import android.widget.Toast;

import java.io.FileOutputStream;
import java.io.IOException;

import static com.canon.ccapisample.Constants.CCAPI.Method.GET;

class ContentsDownloader {
    private static final String TAG = ContentsDownloader.class.getSimpleName();
    private DocumentFile mSaveToDir;
    private ProgressDialogFragment mProgressDialog = null;
    private FileOutputStream mOutputStream = null;
    private final Handler mHandler;

    private static long startTime;
    private static long endTime;

    ContentsDownloader(Handler handler){
        mHandler = handler;
    }

    void execute(final FragmentActivity activity, final String fileName, final String url, final Uri dstUri, final String dialogTitle, final WebAPIResultListener webAPIResultListener){
        mSaveToDir = DocumentFile.fromTreeUri(activity, dstUri);

        final Bundle args = new Bundle();
        String[] params = new String[]{ GET, url, null };
        args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

        Log.d(TAG, "Download Start.");

        // For performance measurement.
        startTime = System.currentTimeMillis();

        if (mSaveToDir != null && mSaveToDir.canWrite()) {
            DocumentFile file = mSaveToDir.findFile(fileName);
            if (file != null) {
                file.delete();
            }
            String mimeType = getMimeTypeForFileName(fileName);
            DocumentFile newFile = mSaveToDir.createFile(mimeType, fileName);
            if (newFile != null) {
                try {
                    if(mOutputStream != null){
                        mOutputStream.close();
                    }

                    mOutputStream = (FileOutputStream) activity.getContentResolver().openOutputStream(newFile.getUri());
                } catch (IOException e) {
                    e.printStackTrace();
                }
            }

            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    mProgressDialog = ProgressDialogFragment.newInstance(ProgressDialogFragment.Type.Bar, dialogTitle, url);
                    FragmentTransaction fragmentTransaction = activity.getSupportFragmentManager().beginTransaction();
                    fragmentTransaction.add(mProgressDialog, null);
                    fragmentTransaction.commitAllowingStateLoss();

                    // Request to get images.
                    // Execute the save processing of images in the onHttpProgressing().
                    WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, new WebAPIResultListener() {
                        @Override
                        public void onWebAPIResult(WebAPIResultDataSet result) {
                            Log.d(TAG, "Download End.");

                            // For performance measurement.
                            endTime = System.currentTimeMillis();
                            if(webAPIResultListener == null) {
                                Toast.makeText(activity, "Download time : " + (endTime - startTime) + " ms", Toast.LENGTH_SHORT).show();
                            }

                            if (mOutputStream != null) {
                                try {
                                    mOutputStream.close();
                                    mOutputStream = null;
                                }
                                catch (IOException e) {
                                    e.printStackTrace();
                                }
                            }

                            if(mProgressDialog != null) {
                                if (result.isError()) {
                                    // The dialog does not close when an error occurred.
                                    String message;
                                    mProgressDialog.stopProgress();

                                    if (result.isCancel()) {
                                        message = "Download Canceled.";
                                    }
                                    else {
                                        message = result.getErrorMsg();
                                    }

                                    DocumentFile file = mSaveToDir.findFile(fileName);
                                    if(!file.delete()){
                                        Log.d(TAG, fileName + " : delete error.");
                                    }
                                    mProgressDialog.setMessage(message);
                                }
                                else {
                                    mProgressDialog.dismissAllowingStateLoss();
                                    mProgressDialog = null;
                                }
                            }

                            if(webAPIResultListener != null) {
                                webAPIResultListener.onWebAPIResult(result);
                            }
                        }
                    }, new HttpProgressListener() {
                        @Override
                        public void onHttpProgressing(int max, int progress, byte[] progressBytes) {

                            if (progressBytes != null && progressBytes.length != 0 && mOutputStream != null){
                                try {
                                    mOutputStream.write(progressBytes);
                                }
                                catch (IOException e) {
                                    e.printStackTrace();
                                }
                            }

                            if (mProgressDialog != null) {
                                mProgressDialog.updateProgress(max, progress);
                            }
                            else{
                                Log.d(TAG, "mProgressDialog is NULL");
                            }
                        }
                    }));
                }
            });
        }
        else {
            Log.d(TAG, "Not exist SaveDirectory.");
        }
    }

    void execute(FragmentActivity context, final String fileName, String url, Uri dstUri){
        execute(context, fileName, url, dstUri, fileName, null);
    }

    private String getMimeTypeForFileName(String fileName) {
        String ext = null;
        int pos = fileName.lastIndexOf(".");
        if (pos >= 0 && pos < fileName.length() -1) {
            ext = fileName.substring(pos + 1);
        }
        if (ext != null) {
            ext = ext.toLowerCase();
        }

        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
    }
}
