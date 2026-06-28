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

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

import static com.canon.ccapisample.Constants.CCAPI.Field.COMMSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Field.COMMSETTING1;
import static com.canon.ccapisample.Constants.CCAPI.Field.COMMSETTING2;
import static com.canon.ccapisample.Constants.CCAPI.Field.FUNCTIONSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Field.FUNCTIONSETTING1;
import static com.canon.ccapisample.Constants.CCAPI.Field.FUNCTIONSETTING2;
import static com.canon.ccapisample.Constants.CCAPI.Method.DELETE;
import static com.canon.ccapisample.Constants.CCAPI.Method.GET;
import static com.canon.ccapisample.Constants.CCAPI.Method.PUT;


/**
 * A simple {@link Fragment} subclass.
 * Use the {@link NetworkConnectionSettingDialogFragment#newInstance} factory method to
 * create an instance of this fragment.
 */
public class NetworkConnectionSettingDialogFragment extends DialogFragment implements WebAPIResultListener {
    private static final String TAG = NetworkConnectionSettingDialogFragment.class.getSimpleName();
    private static final String API_KEY = "ApiKey";
    private WebAPI mWebAPI;
    private APIDataSet mApiDataSet;

    private Map<String, Integer> mViewIDMap = new HashMap<String, Integer>() {
        {
            put(COMMSETTING1, R.id.CommSetting1Spinner);
            put(COMMSETTING2, R.id.CommSetting2Spinner );
            put(FUNCTIONSETTING1, R.id.FunctionSetting1Spinner );
            put(FUNCTIONSETTING2, R.id.FunctionSetting2Spinner );
        }
    };

    public NetworkConnectionSettingDialogFragment() {
        // Required empty public constructor
    }

    public static NetworkConnectionSettingDialogFragment newInstance(Fragment target, String key) {
        NetworkConnectionSettingDialogFragment fragment = new NetworkConnectionSettingDialogFragment();
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

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_network_connection_setting, null);
        LinearLayout layout = view.findViewById(R.id.NetworkConnectionSetting);

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
        JSONArray arr1 = new JSONArray();
        JSONArray arr2 = new JSONArray();

        arr1.put(getValue(COMMSETTING1));
        arr1.put(getValue(COMMSETTING2));
        arr2.put(getValue(FUNCTIONSETTING1));
        arr2.put(getValue(FUNCTIONSETTING2));
        try {
            body.put("commsetting", arr1);
            body.put("functionsetting", arr2);
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
                    case COMMSETTING1:
                    case COMMSETTING2:
                    case FUNCTIONSETTING1:
                    case FUNCTIONSETTING2:
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
                    JSONArray value = response.getJSONArray(name);

                    if(name.equals(COMMSETTING)){
                        for(int i = 1; i <= 2; i++){
                            String name2 = name + i;
                            int id = mViewIDMap.get(name2);
                            Spinner spinner = view.findViewById(id);
                            String nw = value.getString(i-1);
                            int j = 1;
                            while(j < spinner.getCount()){
                                if (spinner.getItemAtPosition(j).toString().equals(nw)){
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
                    if(name.equals(FUNCTIONSETTING)){
                        for(int i = 1; i <= 2; i++){
                            String name2 = name + i;
                            int id = mViewIDMap.get(name2);
                            Spinner spinner = view.findViewById(id);
                            String nw = value.getString(i-1);
                            int j = 1;
                            while(j < spinner.getCount()){
                                if (spinner.getItemAtPosition(j).toString().equals(nw)){
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
                }
            }
            catch (JSONException e) {
                e.printStackTrace();
            }

        }
    }

}
