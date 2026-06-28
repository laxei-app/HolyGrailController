package com.canon.ccapisample;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.net.Uri;
import android.os.Bundle;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.View;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static com.canon.ccapisample.Constants.CCAPI.Field.ACTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.VALUE;
import static com.canon.ccapisample.Constants.CCAPI.Method.PUT;
import static com.canon.ccapisample.Constants.CCAPI.Value.XMP;

public class ContentsXMPSettingDialogFragment extends DialogFragment {

    private static final String TAG = ContentsXMPSettingDialogFragment.class.getSimpleName();
    private static final String STORAGE_CONTENTS_DATA_SET = "ContentsDataSet";
    private WebAPI mWebAPI;
    private ContentsDataSet mContentsDataSet;
    private Uri mSaveDirUri;
    private ListViewDataSet mListViewDataSet;
    private final Map<String, EditText> mXMPStringViewMap = new HashMap<>();

    public static ContentsXMPSettingDialogFragment newInstance(Fragment target, ContentsDataSet contentsDataSet){
        ContentsXMPSettingDialogFragment instance = new ContentsXMPSettingDialogFragment();
        instance.setTargetFragment(target, 0);
        Bundle arguments = new Bundle();
        arguments.putSerializable(STORAGE_CONTENTS_DATA_SET, contentsDataSet);
        instance.setArguments(arguments);
        return instance;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        Log.d(TAG, "onCreateDialog");

        if (getArguments() != null) {
            mContentsDataSet = (ContentsDataSet) getArguments().getSerializable(STORAGE_CONTENTS_DATA_SET);
        }

        mWebAPI = WebAPI.getInstance();

        LinearLayout parentLayout = new LinearLayout(getActivity());
        parentLayout.setOrientation(LinearLayout.VERTICAL);

        AlertDialog.Builder dialog = new AlertDialog.Builder(getActivity());

        dialog.setTitle(XMP);

        Context context = getActivity();

        LinearLayout subLayout = new LinearLayout(context);
        subLayout.setOrientation(LinearLayout.HORIZONTAL);

        EditText editText = new EditText(getActivity());
        subLayout.addView(editText, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 2));

        mXMPStringViewMap.put(XMP, editText);

        parentLayout.addView(subLayout, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));

        dialog.setPositiveButton("Set", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                putXMPAction();
            }
        });

        dialog.setView(parentLayout);

        return dialog.create();
    }

    @Override
    public void onStart() {
        super.onStart();

        // The minHeight in the custom layout of dialogs is 48dp.
        // Therefore unnecessary space is displayed.
        // So, disable it immediately before displaying a dialog.
        if(getDialog() != null) {
            FrameLayout frameLayout = getDialog().findViewById(android.R.id.custom);
            if (frameLayout != null && frameLayout.getParent() != null) {
                ((FrameLayout) frameLayout.getParent()).setMinimumHeight(0);
            }
        }
    }

    @Override
    public void onPause() {
        super.onPause();
        dismiss();
    }

    private void putXMPAction(){
        JSONObject body = new JSONObject();
        String data = new String();
        try {
            body.put(ACTION, XMP);
            data += mXMPStringViewMap.get(XMP).getText().toString();
            body.put(VALUE,data);
        }
        catch (JSONException e) {
            e.printStackTrace();
        }

        Log.d(TAG, body.toString());
        Bundle args = new Bundle();
        String[] params = new String[]{PUT, mContentsDataSet.getUrl(), body.toString()};
        args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, (WebAPIResultListener) getTargetFragment()));
    }

}


