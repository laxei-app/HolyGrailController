package com.canon.ccapisample;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.support.v4.provider.DocumentFile;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

import static android.app.Activity.RESULT_OK;
import static com.canon.ccapisample.Constants.CCAPI.Field.SSL_CACERT;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_SSL_CACERT;

public class SslCacertDialogFragment extends DialogFragment implements View.OnClickListener {
    private static final String TAG = SslCacertDialogFragment.class.getSimpleName();

    private WebAPI mWebAPI;
    private Button mSelectButton;
    private Button mSaveButton;
    private TextView mSaveDirectoryText;

    private final List<ContentsDataSet> mContentsDataSetList = new ArrayList<>();
    private ApplicationInfo mAppInfo;

    private static final int OPEN_DOCUMENT_REQUEST_CODE = 100;


    public static SslCacertDialogFragment newInstance(Fragment target) {
        SslCacertDialogFragment fragment = new SslCacertDialogFragment();
        fragment.setTargetFragment(target, 0);
        return fragment;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        AlertDialog.Builder dialog = new AlertDialog.Builder(getActivity());
        mWebAPI = WebAPI.getInstance();
        mAppInfo = (ApplicationInfo)getActivity().getApplication();

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_ssl_cacert, null);

        LinearLayout layout = view.findViewById(R.id.SslCacert);
        view.findViewById(R.id.SelectButton).setOnClickListener(this);
        view.findViewById(R.id.SaveButton).setOnClickListener(this);

        mSelectButton = view.findViewById(R.id.SelectButton);
        mSaveButton = view.findViewById(R.id.SaveButton);
        mSaveDirectoryText = view.findViewById(R.id.SaveDirectory);

        //Disable Save button if no directory is selected.
        Uri uri = mAppInfo.getSavePathUri();
        if (uri != null) {
            DocumentFile file = DocumentFile.fromTreeUri(getContext(), uri);
            mSaveDirectoryText.setText(file.getName());
        }
        if (mSaveDirectoryText.getText().toString().equals("")) {
            mSaveButton.setEnabled(false);
        }

        dialog.setTitle(SSL_CACERT);
        dialog.setView(layout);
        dialog.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int id) {
            }
        });

        return dialog.create();
    }

    @Override
    public void onClick(View v){
        if (v != null) {
            switch (v.getId()) {
                case R.id.SelectButton:
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    this.startActivityForResult(intent, OPEN_DOCUMENT_REQUEST_CODE);
                    break;
                case R.id.SaveButton:
                    CacertDownloaderThread thread = new CacertDownloaderThread(mContentsDataSetList, mAppInfo.getSavePathUri());
                    thread.start();
                    break;
                default:
                    break;
            }
        }
    }

    public class CacertDownloaderThread extends Thread {
        private final List<ContentsDataSet> mContentsDataSetList;
        private final Handler mHandler;
        private final Uri mSaveDirectoryUri;
        private final String name = SSL_CACERT;
        private final String FILE_NAME = "cacert.crt";

        CacertDownloaderThread(List<ContentsDataSet> contentsDataSetList, Uri saveDirectoryUri) {
            mContentsDataSetList = contentsDataSetList;
            mHandler = new Handler();
            mSaveDirectoryUri = saveDirectoryUri;
        }

        private void notifyThread(){
            synchronized (this) {
                this.notifyAll();
            }
        }

        @Override
        public void run() {
            final APIDataSet api = mWebAPI.getAPIData(FUNCTIONS_SSL_CACERT);

            DocumentFile mSaveToDir = DocumentFile.fromTreeUri(getActivity(), mSaveDirectoryUri);
            if (mSaveToDir == null || mSaveToDir.canWrite() == false) {
                Log.d(TAG, "Not exist SaveDirectory.");
                return;
            }

            ContentsDownloader contentsDownloader = new ContentsDownloader(mHandler);

            Log.d(TAG, "CacertDownloaderThread begin.");

            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    Log.d(TAG, "CacertDownloaderThread window not touchable.");
                    if(getActivity() != null) {
                        getActivity().getWindow().setFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
                    }
                }
            });

            // Display a dialog -> Get or save a file  -> Remove the dialog
            // Do nothing, if the activity is discarded.
            if(getActivity() != null) {
                SslCacertDialogFragment.this.setCancelable(false);
                contentsDownloader.execute(getActivity(), FILE_NAME, api.getUrl(), mSaveDirectoryUri, name, new WebAPIResultListener() {
                    @Override
                    public void onWebAPIResult(WebAPIResultDataSet result) {
                        Context context = getActivity();
                        SslCacertDialogFragment.this.setCancelable(true);
                        if (result.isError()) {
                            Toast.makeText(context, "Save Failure.", Toast.LENGTH_SHORT).show();
                            interrupt();
                        } else {
                            //Added a download completion notification because the download process was too fast.
                            Toast.makeText(context, "Save Success.", Toast.LENGTH_SHORT).show();
                            notifyThread();
                        }
                    }
                });

                try {
                    // Wait until the above series of processing is completed.
                    Log.d(TAG, "CacertDownloaderThread wait.");
                    synchronized (this) {
                        this.wait();
                    }
                    Log.d(TAG, "CacertDownloaderThread resume.");
                }
                catch (InterruptedException e) {
                    // Terminate the processing, if an error occurs in the middle.
                    Log.d(TAG, "CacertDownloaderThread InterruptedException.");
                    e.printStackTrace();
                }
            }

            Log.d(TAG, "CacertDownloaderThread end.");

            // Cancel prohibition of screen operation.
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    Log.d(TAG, "CacertDownloaderThread window touchable.");
                    if(getActivity() != null) {
                        getActivity().getWindow().clearFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
                    }
                }
            });
        }
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == OPEN_DOCUMENT_REQUEST_CODE) {
            if (resultCode == RESULT_OK) {
                Uri uri = null;
                if (data != null) {
                    uri = data.getData();
                }

                if (uri != null) {
                    mAppInfo.setSavePathUri(uri);
                    DocumentFile file = DocumentFile.fromTreeUri(getContext(), uri);
                    mSaveDirectoryText.setText(file.getName());
                    mSaveButton.setEnabled(true);
                }
                else {
                    mSaveButton.setEnabled(false);
                }
            }
        }
    }


}
