package com.canon.ccapisample;


import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

import static com.canon.ccapisample.Constants.CCAPI.Field.COMMFUNCTION;
import static com.canon.ccapisample.Constants.CCAPI.Method.DELETE;
import static com.canon.ccapisample.Constants.CCAPI.Method.GET;
import static com.canon.ccapisample.Constants.CCAPI.Method.PUT;


/**
 * A simple {@link Fragment} subclass.
 * Use the {@link NetworkFunctionSettingDialogFragment#newInstance} factory method to
 * create an instance of this fragment.
 */
public class NetworkFunctionSettingDialogFragment extends DialogFragment implements WebAPIResultListener {
    private static final String TAG = NetworkFunctionSettingDialogFragment.class.getSimpleName();
    private static final String API_KEY = "ApiKey";
    private WebAPI mWebAPI;
    private APIDataSet mApiDataSet;

    private Map<String, Integer> mViewIDMap = new HashMap<String, Integer>() {
        {
            put(COMMFUNCTION, R.id.NetworkFunctionSetting1Spinner);
        }
    };

    public NetworkFunctionSettingDialogFragment() {
        // Required empty public constructor
    }

    public static NetworkFunctionSettingDialogFragment newInstance(Fragment target, String key) {
        NetworkFunctionSettingDialogFragment fragment = new NetworkFunctionSettingDialogFragment();
        fragment.setTargetFragment(target, 0);
        Bundle args = new Bundle();
        args.putString(API_KEY, key);
        fragment.setArguments(args);
        return fragment;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        AlertDialog.Builder dialog = new AlertDialog.Builder(getActivity());
        mWebAPI = WebAPI.getInstance();
        String apiKey = getArguments().getString(API_KEY);
        mApiDataSet = mWebAPI.getAPIData(apiKey);

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_network_function_setting, null);
        LinearLayout layout = view.findViewById(R.id.NetworkFunctionSetting);

        // Get current values.
        if (mApiDataSet != null && mApiDataSet.isGetable()) {
            Bundle args = new Bundle();
            String[] params = new String[]{GET, mApiDataSet.getUrl(), null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
        }

        dialog.setTitle(apiKey);
        dialog.setView(layout);
        dialog.setPositiveButton("Set", new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface dialog, int id) {
                setConnectionSetting();
            }
        });
        dialog.setNegativeButton("Reset", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                resetConnectionSetting();
            }
        });

        return dialog.create();
    }

    @Override
    public void onStop() {
        super.onStop();
        Log.d(TAG, "onStop");
    }

    /**
     * Callback from execute WebAPI
     * @param result HTTP Request result
     */
    @Override
    public void onWebAPIResult(WebAPIResultDataSet result){
        Log.d(TAG, String.format("%s onWebAPIResult", String.valueOf(result.getRequestCode())));
        Context context = getActivity();

        // Do nothing, if life cycle of the fragment is finished.
        if(context != null) {
            if (result.isError()) {
                Toast.makeText(context, result.getErrorMsg(), Toast.LENGTH_SHORT).show();
            } else {
                switch (result.getRequestCode()) {
                    case ACT_WEB_API:
                        setValue(result.getResponseBody());
                        break;
                    default:
                        break;
                }
            }
        }
        else{
            Log.d(TAG, String.format("%s Activity is Null.", String.valueOf(result.getRequestCode())));
        }
    }

    private void setConnectionSetting(){
        JSONObject body = new JSONObject();

        try {
            body.put(COMMFUNCTION, getValue(COMMFUNCTION));
        } catch (JSONException e) {
            e.printStackTrace();
        }

        Log.d(TAG, body.toString());

        if (mApiDataSet != null && mApiDataSet.isPutable()) {
            Bundle args = new Bundle();
            String[] params = new String[]{PUT, mApiDataSet.getUrl(), body.toString()};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

            // Execute the API.
            // The onWebAPIResult() of caller Fragment process the execution result, because the dialog is closed.
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, new WebAPIResultListener() {
                @Override
                public void onWebAPIResult(WebAPIResultDataSet result) {
                    if (getTargetFragment() instanceof WebAPIResultListener) {
                        ((WebAPIResultListener) getTargetFragment()).onWebAPIResult(result);
                    }
                }
            }));
        }
    }

    private void resetConnectionSetting(){

        // Execute the API.
        // The onWebAPIResult() of caller Fragment process the execution result, because the dialog is closed.
        if (mApiDataSet != null && mApiDataSet.isDeletable()) {
            Bundle args = new Bundle();
            String[] params = new String[]{DELETE, mApiDataSet.getUrl(), null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

            // Execute the API.
            // The onWebAPIResult() of caller Fragment process the execution result, because the dialog is closed.
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, new WebAPIResultListener() {
                @Override
                public void onWebAPIResult(WebAPIResultDataSet result) {
                    if (getTargetFragment() instanceof WebAPIResultListener) {
                        ((WebAPIResultListener) getTargetFragment()).onWebAPIResult(result);
                    }
                }
            }));
        }
    }

    private String getValue(String name){
        String value = "";
        Dialog view = getDialog();

        if(view != null) {
            int id = mViewIDMap.get(name);
            View tempView = view.findViewById(id);

            if (tempView.isShown()) {
                switch (name) {
                    case COMMFUNCTION:
                        Spinner spinner = (Spinner) tempView;
                        value = spinner.getSelectedItem().toString();
                        break;

                    default:
                        break;
                }
            }
            else {
                value = "";
            }
        }

        return value;
    }

    private void setValue(String responseBody){
        Dialog view = getDialog();

        if(view != null) {
            try {
                JSONObject response = new JSONObject(responseBody);
                Iterator<String> keys = response.keys();

                // Reflect the current value in the View.
                while (keys.hasNext()) {
                    String name = keys.next();
                    String value = response.getString(name);
                    int id = mViewIDMap.get(name);
                    Spinner spinner = view.findViewById(id);
                    int j = 1;
                    while(j < spinner.getCount()){
                        if (spinner.getItemAtPosition(j).toString().equals(value)){
                            spinner.setSelection(j);
                            break;
                        }
                        j++;
                        if(j == spinner.getCount()){
                            spinner.setSelection(0);
                            break;
                        }
                    }
                }
            }
            catch (JSONException e) {
                e.printStackTrace();
            }

        }
    }

}
