package com.canon.ccapisample;


import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.support.annotation.IdRes;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

import static com.canon.ccapisample.Constants.CCAPI.Field.AUTHENTICATION;
import static com.canon.ccapisample.Constants.CCAPI.Field.CHANNEL;
import static com.canon.ccapisample.Constants.CCAPI.Field.ENCRYPTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV4_GATEWAY;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV4_IPADDRESS;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV4_IPADDRESSSET;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV4_SUBNETMASK;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV6_GATEWAY;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV6_MANUAL_ADDRESS;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV6_MANUAL_SETTING;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV6_PREFIXLENGTH;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPV6_USEIPV6;
import static com.canon.ccapisample.Constants.CCAPI.Field.KEYINDEX;
import static com.canon.ccapisample.Constants.CCAPI.Field.LANTYPE;
import static com.canon.ccapisample.Constants.CCAPI.Field.METHOD;
import static com.canon.ccapisample.Constants.CCAPI.Field.PASSWORD;
import static com.canon.ccapisample.Constants.CCAPI.Field.SSID;
import static com.canon.ccapisample.Constants.CCAPI.Method.DELETE;
import static com.canon.ccapisample.Constants.CCAPI.Method.GET;
import static com.canon.ccapisample.Constants.CCAPI.Method.PUT;
import static com.canon.ccapisample.Constants.CCAPI.Value.AES;
import static com.canon.ccapisample.Constants.CCAPI.Value.AUTO;
import static com.canon.ccapisample.Constants.CCAPI.Value.CAMERAAP;
import static com.canon.ccapisample.Constants.CCAPI.Value.DISABLE;
import static com.canon.ccapisample.Constants.CCAPI.Value.ENABLE;
import static com.canon.ccapisample.Constants.CCAPI.Value.INFRASTRUCTURE;
import static com.canon.ccapisample.Constants.CCAPI.Value.MANUAL;
import static com.canon.ccapisample.Constants.CCAPI.Value.NONE;
import static com.canon.ccapisample.Constants.CCAPI.Value.OPEN;
import static com.canon.ccapisample.Constants.CCAPI.Value.SHAREDKEY;
import static com.canon.ccapisample.Constants.CCAPI.Value.TKIPAES;
import static com.canon.ccapisample.Constants.CCAPI.Value.WEP;
import static com.canon.ccapisample.Constants.CCAPI.Value.WPAWPA2PSK;


/**
 * A simple {@link Fragment} subclass.
 * Use the {@link NetworkCommSettingDialogFragment#newInstance} factory method to
 * create an instance of this fragment.
 */
public class NetworkCommSettingDialogFragment extends DialogFragment implements WebAPIResultListener, RadioGroup.OnCheckedChangeListener {
    private static final String TAG = NetworkCommSettingDialogFragment.class.getSimpleName();
    private static final String API_KEY = "ApiKey";
    private WebAPI mWebAPI;
    private APIDataSet mApiDataSet;

    private Map<String, Integer> mViewIDMap = new HashMap<String, Integer>() {
        {
            put(LANTYPE, R.id.comm_lantypeSpinner);
            put(IPV4_IPADDRESSSET, R.id.Comm_Ipv4_IpaddresssetRadio );
            put(IPV4_IPADDRESS, R.id.ipv4_ipaddressEdit );
            put(IPV4_SUBNETMASK, R.id.ipv4_subnetmaskEdit );
            put(IPV4_GATEWAY, R.id.ipv4_gatewayEdit );
            put(IPV6_USEIPV6, R.id.Comm_Ipv6_UseIpv6Radio);
            put(IPV6_MANUAL_SETTING, R.id.Comm_Ipv6_Manual_SettingRadio);
            put(IPV6_MANUAL_ADDRESS, R.id.ipv6_manual_addressEdit );
            put(IPV6_PREFIXLENGTH, R.id.ipv6_prefixlengthEdit );
            put(IPV6_GATEWAY, R.id.ipv6_gatewayEdit );
            put(SSID, R.id.comm_ssidEdit );
            put(METHOD, R.id.comm_methodRadio );
            put(CHANNEL, R.id.comm_channelSpinner );
            put(AUTHENTICATION, R.id.comm_authenticationRadio );
            put(ENCRYPTION, R.id.comm_encryptionRadio );
            put(KEYINDEX, R.id.comm_keyindexSpinner );
            put(PASSWORD, R.id.comm_passwordEdit );
        }
    };

    private Map<String, Integer> mMethodRadioIDMap = new HashMap<String, Integer>() {
        {
            put(INFRASTRUCTURE, R.id.Comm_InfrastructureRadioButton );
            put(CAMERAAP, R.id.Comm_CameraapRadioButton );
        }
    };

    private Map<String, Integer> mAuthenticationRadioIDMap = new HashMap<String, Integer>() {
        {
            put(OPEN, R.id.Comm_OpenRadioButton );
            put(SHAREDKEY, R.id.Comm_SharedKeyRadioButton );
            put(WPAWPA2PSK, R.id.Comm_Wpawpa2pskRadioButton );
        }
    };

    private Map<String, Integer> mEncryptionRadioIDMap = new HashMap<String, Integer>() {
        {
            put(NONE, R.id.Comm_NoneRadioButton );
            put(WEP, R.id.Comm_WepRadioButton );
            put(TKIPAES, R.id.Comm_TkipaesRadioButton );
            put(AES, R.id.Comm_AesRadioButton );
        }
    };

    private Map<String, Integer> mIpaddresssetRadioIDMap = new HashMap<String, Integer>() {
        {
            put(AUTO, R.id.Comm_AutoRadioButton );
            put(MANUAL, R.id.Comm_ManualRadioButton );
        }
    };

    private Map<String, Integer> mUseIpv6RadioIDMap = new HashMap<String, Integer>() {
        {
            put(DISABLE, R.id.Comm_UseIpv6_DisableRadioButton );
            put(ENABLE, R.id.Comm_UseIpv6_EnableRadioButton );
        }
    };

    private Map<String, Integer> mIpv6ManualSetRadioIDMap = new HashMap<String, Integer>() {
        {
            put(DISABLE, R.id.Comm_Manual_Setting_DisableRadioButton );
            put(ENABLE, R.id.Comm_Manual_Setting_EnableRadioButton );
        }
    };

    private Map<String, Map<String, Integer>> mRadioIDMap = new HashMap<String, Map<String, Integer>>(){
        {
            put(METHOD, mMethodRadioIDMap );
            put(AUTHENTICATION, mAuthenticationRadioIDMap );
            put(ENCRYPTION, mEncryptionRadioIDMap );
            put(IPV4_IPADDRESSSET, mIpaddresssetRadioIDMap );
            put(IPV6_USEIPV6, mUseIpv6RadioIDMap );
            put(IPV6_MANUAL_SETTING, mIpv6ManualSetRadioIDMap );
        }
    };

    public NetworkCommSettingDialogFragment() {
        // Required empty public constructor
    }

    public static NetworkCommSettingDialogFragment newInstance(Fragment target, String key) {
        NetworkCommSettingDialogFragment fragment = new NetworkCommSettingDialogFragment();
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

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_network_comm_setting, null);
        LinearLayout layout = view.findViewById(R.id.NetworkCommSetting);

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
                setCommSetting();
            }
        });
        dialog.setNegativeButton("Reset", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                resetCommSetting();
            }
        });

        RadioGroup methodRadio = view.findViewById(R.id.comm_methodRadio);
        methodRadio.setOnCheckedChangeListener(this);
        RadioGroup authenticationRadio = view.findViewById(R.id.comm_authenticationRadio);
        authenticationRadio.setOnCheckedChangeListener(this);
        RadioGroup encryptionRadio = view.findViewById(R.id.comm_encryptionRadio);
        encryptionRadio.setOnCheckedChangeListener(this);
        RadioGroup ipAddressSetRadio = view.findViewById(R.id.Comm_Ipv4_IpaddresssetRadio);
        ipAddressSetRadio.setOnCheckedChangeListener(this);
        RadioGroup useipv6Radio = view.findViewById(R.id.Comm_Ipv6_UseIpv6Radio);
        useipv6Radio.setOnCheckedChangeListener(this);
        RadioGroup ipv6manualsetRadio = view.findViewById(R.id.Comm_Ipv6_Manual_SettingRadio);
        ipv6manualsetRadio.setOnCheckedChangeListener(this);

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

    private void setCommSetting(){
        JSONObject body = new JSONObject();
        for (String name : mViewIDMap.keySet()) {
            try {
                body.put(name, getValue(name));
            }
            catch (JSONException e) {
                e.printStackTrace();
            }
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

    private void resetCommSetting(){

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
                    case SSID:
                    case PASSWORD:
                    case IPV4_IPADDRESS:
                    case IPV4_SUBNETMASK:
                    case IPV4_GATEWAY:
                    case IPV6_MANUAL_ADDRESS:
                    case IPV6_PREFIXLENGTH:
                    case IPV6_GATEWAY:
                        EditText editText = (EditText) tempView;
                        value = editText.getText().toString();
                        break;

                    case METHOD:
                    case AUTHENTICATION:
                    case ENCRYPTION:
                    case IPV4_IPADDRESSSET:
                    case IPV6_USEIPV6:
                    case IPV6_MANUAL_SETTING:
                        RadioGroup radioGroup = (RadioGroup) tempView;
                        int radioID = radioGroup.getCheckedRadioButtonId();

                        for (String key : mRadioIDMap.get(name).keySet()) {
                            if (radioID == mRadioIDMap.get(name).get(key)) {
                                value = key;
                                break;
                            }
                        }
                        break;

                    case CHANNEL:
                    case KEYINDEX:
                    case LANTYPE:
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

                    if(mViewIDMap.containsKey(name)){
                        int id = mViewIDMap.get(name);
                        switch (name) {
                            case SSID:
                            case PASSWORD:
                            case IPV4_IPADDRESS:
                            case IPV4_SUBNETMASK:
                            case IPV4_GATEWAY:
                            case IPV6_MANUAL_ADDRESS:
                            case IPV6_PREFIXLENGTH:
                            case IPV6_GATEWAY:
                                EditText editText = view.findViewById(id);
                                editText.setText(value);
                                break;

                            case METHOD:
                            case AUTHENTICATION:
                            case ENCRYPTION:
                            case IPV4_IPADDRESSSET:
                            case IPV6_USEIPV6:
                            case IPV6_MANUAL_SETTING:
                                RadioGroup radioGroup = view.findViewById(id);
                                if(mRadioIDMap.containsKey(name)){
                                    for (String key : mRadioIDMap.get(name).keySet()) {
                                        if(key.equals(value)){
                                            radioGroup.check(mRadioIDMap.get(name).get(key));
                                            break;
                                        }
                                    }
                                }
                                break;

                            case CHANNEL:
                            case KEYINDEX:
                            case LANTYPE:
                                Spinner spinner = view.findViewById(id);
                                for (int i = 0; i < spinner.getCount(); i++){
                                    if (spinner.getItemAtPosition(i).toString().equals(value)){
                                        spinner.setSelection(i);
                                        break;
                                    }
                                }
                                break;

                            default:
                                break;
                        }
                    }
                }
            }
            catch (JSONException e) {
                e.printStackTrace();
            }

            // Change Visibility of the View in accordance with the current value.
            setViewVisibility(view);

        }
    }

    @Override
    public void onCheckedChanged(RadioGroup group, @IdRes int checkedId) {
        Log.d(TAG, "onCheckedChanged");
        Dialog view = getDialog();
        if(view != null) {
            switch (group.getId()) {
                case R.id.comm_methodRadio:
                case R.id.comm_authenticationRadio:
                case R.id.comm_encryptionRadio:
                case R.id.Comm_Ipv4_IpaddresssetRadio:
                case R.id.Comm_Ipv6_UseIpv6Radio:
                case R.id.Comm_Ipv6_Manual_SettingRadio:
                    // Change Visibility of the View in accordance with the current value.
                    setViewVisibility(view);
                    break;
                default:
                    break;
            }
        }
    }

    private void setViewVisibility(Dialog view){
        RadioGroup methodRadio = view.findViewById(R.id.comm_methodRadio);
        RadioGroup authenticationRadio = view.findViewById(R.id.comm_authenticationRadio);
        RadioGroup encryptionRadio = view.findViewById(R.id.comm_encryptionRadio);
        RadioGroup ipAddressSetRadio = view.findViewById(R.id.Comm_Ipv4_IpaddresssetRadio);
        RadioGroup ipv6_useipv6Radio = view.findViewById(R.id.Comm_Ipv6_UseIpv6Radio);
        RadioGroup ipv6_manual_settingRadio = view.findViewById(R.id.Comm_Ipv6_Manual_SettingRadio);

        int methodID = methodRadio.getCheckedRadioButtonId();
        int authenticationID = authenticationRadio.getCheckedRadioButtonId();
        int encryptionID = encryptionRadio.getCheckedRadioButtonId();
        int ipAddressSetID = ipAddressSetRadio.getCheckedRadioButtonId();
        int ipv6_useipv6ID = ipv6_useipv6Radio.getCheckedRadioButtonId();
        int ipv6_manual_settingID = ipv6_manual_settingRadio.getCheckedRadioButtonId();

        // Change the display of channel, authentication, encryption in accordance with the methodID.
        if(methodID == R.id.Comm_InfrastructureRadioButton){
            view.findViewById(R.id.Comm_ChannelLayout).setVisibility(View.GONE);
            //view.findViewById(R.id.Comm_AuthenticationLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.Comm_KeyIndexLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.Comm_EncryptionLayout).setVisibility(View.VISIBLE);

            // Change the display of the encryption RadioButton in accordance with the authenticationID.
            if(authenticationID == R.id.Comm_OpenRadioButton){
                view.findViewById(R.id.Comm_NoneRadioButton).setEnabled(true);
                view.findViewById(R.id.Comm_WepRadioButton).setEnabled(true);
                view.findViewById(R.id.Comm_TkipaesRadioButton).setEnabled(false);
                view.findViewById(R.id.Comm_AesRadioButton).setEnabled(false);
            }
            else if(authenticationID == R.id.Comm_SharedKeyRadioButton){
                view.findViewById(R.id.Comm_NoneRadioButton).setEnabled(false);
                view.findViewById(R.id.Comm_WepRadioButton).setEnabled(true);
                view.findViewById(R.id.Comm_TkipaesRadioButton).setEnabled(false);
                view.findViewById(R.id.Comm_AesRadioButton).setEnabled(false);
            }
            else {
                view.findViewById(R.id.Comm_NoneRadioButton).setEnabled(false);
                view.findViewById(R.id.Comm_WepRadioButton).setEnabled(false);
                view.findViewById(R.id.Comm_TkipaesRadioButton).setEnabled(true);
                view.findViewById(R.id.Comm_AesRadioButton).setEnabled(false);
            }
        }
        else{
            view.findViewById(R.id.Comm_ChannelLayout).setVisibility(View.VISIBLE);
            //view.findViewById(R.id.Comm_AuthenticationLayout).setVisibility(View.GONE);
            view.findViewById(R.id.Comm_KeyIndexLayout).setVisibility(View.GONE);
            view.findViewById(R.id.Comm_NoneRadioButton).setEnabled(true);
            view.findViewById(R.id.Comm_WepRadioButton).setEnabled(false);
            view.findViewById(R.id.Comm_TkipaesRadioButton).setEnabled(false);
            view.findViewById(R.id.Comm_AesRadioButton).setEnabled(true);
        }

        if(encryptionID != -1 && !view.findViewById(encryptionID).isEnabled()){
            if(view.findViewById(R.id.Comm_NoneRadioButton).isEnabled()) {
                ((RadioButton) view.findViewById(R.id.Comm_NoneRadioButton)).setChecked(true);
            }
            else if(view.findViewById(R.id.Comm_WepRadioButton).isEnabled()) {
                ((RadioButton) view.findViewById(R.id.Comm_WepRadioButton)).setChecked(true);
            }
            else if(view.findViewById(R.id.Comm_TkipaesRadioButton).isEnabled()){
                ((RadioButton) view.findViewById(R.id.Comm_TkipaesRadioButton)).setChecked(true);
            }
            encryptionID = encryptionRadio.getCheckedRadioButtonId();
        }

        // Change the display of keyindex, password in accordance with the encryptionID.
        if (encryptionID == R.id.Comm_NoneRadioButton) {
            view.findViewById(R.id.Comm_KeyIndexLayout).setVisibility(View.GONE);
            view.findViewById(R.id.comm_passwordEdit).setEnabled(false);
            view.findViewById(R.id.Comm_PasswordLayout).setVisibility(View.GONE);
        }
        else{
            if (encryptionID == R.id.Comm_WepRadioButton) {
                view.findViewById(R.id.Comm_KeyIndexLayout).setVisibility(View.VISIBLE);
            }
            else{
                view.findViewById(R.id.Comm_KeyIndexLayout).setVisibility(View.GONE);
            }
            view.findViewById(R.id.comm_passwordEdit).setEnabled(true);
            view.findViewById(R.id.Comm_PasswordLayout).setVisibility(View.VISIBLE);
        }

        // Change the display of ipaddress, subnetmask, gateway in accordance with the ipaddresssetID.
        if(ipAddressSetID == R.id.Comm_AutoRadioButton){
            view.findViewById(R.id.ipv4_ipaddressEdit).setEnabled(false);
            view.findViewById(R.id.ipv4_subnetmaskEdit).setEnabled(false);
            view.findViewById(R.id.ipv4_gatewayEdit).setEnabled(false);
            view.findViewById(R.id.ipv4_ipaddressLayout).setVisibility(View.GONE);
            view.findViewById(R.id.ipv4_subnetmaskLayout).setVisibility(View.GONE);
            view.findViewById(R.id.ipv4_gatewayLayout).setVisibility(View.GONE);
        }
        else{
            view.findViewById(R.id.ipv4_ipaddressEdit).setEnabled(true);
            view.findViewById(R.id.ipv4_subnetmaskEdit).setEnabled(true);
            view.findViewById(R.id.ipv4_gatewayEdit).setEnabled(true);
            view.findViewById(R.id.ipv4_ipaddressLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.ipv4_subnetmaskLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.ipv4_gatewayLayout).setVisibility(View.VISIBLE);
        }

        if(ipv6_useipv6ID == R.id.Comm_UseIpv6_DisableRadioButton){
            view.findViewById(R.id.Comm_Ipv6_Manual_SettingLayout).setVisibility(View.GONE);
            view.findViewById(R.id.ipv6_manual_addressEdit).setEnabled(false);
            view.findViewById(R.id.ipv6_prefixlengthEdit).setEnabled(false);
            view.findViewById(R.id.ipv6_gatewayEdit).setEnabled(false);
            view.findViewById(R.id.Comm_Ipv6_Manual_AddressLayout).setVisibility(View.GONE);
            view.findViewById(R.id.Comm_Ipv6_PrefixLengthLayout).setVisibility(View.GONE);
            view.findViewById(R.id.Comm_Ipv6_GatewayLayout).setVisibility(View.GONE);

        }else{
            view.findViewById(R.id.Comm_Ipv6_Manual_SettingLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.ipv6_manual_addressEdit).setEnabled(true);
            view.findViewById(R.id.ipv6_prefixlengthEdit).setEnabled(true);
            view.findViewById(R.id.ipv6_gatewayEdit).setEnabled(true);
            view.findViewById(R.id.Comm_Ipv6_Manual_AddressLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.Comm_Ipv6_PrefixLengthLayout).setVisibility(View.VISIBLE);
            view.findViewById(R.id.Comm_Ipv6_GatewayLayout).setVisibility(View.VISIBLE);

        }

        if(ipv6_manual_settingID == R.id.Comm_Manual_Setting_EnableRadioButton){
            view.findViewById(R.id.ipv6_manual_addressEdit).setEnabled(true);
            view.findViewById(R.id.Comm_Ipv6_Manual_AddressLayout).setVisibility(View.VISIBLE);
        }else{
            view.findViewById(R.id.ipv6_manual_addressEdit).setEnabled(false);
            view.findViewById(R.id.Comm_Ipv6_Manual_AddressLayout).setVisibility(View.GONE);
        }

    }

}
