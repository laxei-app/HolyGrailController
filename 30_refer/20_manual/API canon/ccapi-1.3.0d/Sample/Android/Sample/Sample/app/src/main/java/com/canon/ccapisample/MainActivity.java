package com.canon.ccapisample;

import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.AsyncTask;
import android.os.Handler;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.FragmentManager;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.Toast;

import java.net.MalformedURLException;
import java.net.URL;

import static com.canon.ccapisample.Constants.CCAPI.Key.EVENT_POLLING;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_NETWORKCONNECTION;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_NETWORKSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Method.GET;
import static com.canon.ccapisample.Constants.CCAPI.VER110;


public class MainActivity extends AppCompatActivity implements View.OnClickListener, AuthenticateListener, WebAPIResultListener, DisconnectListener {
    private static final String TAG = MainActivity.class.getSimpleName();
    private static final int REQUEST_CODE = 1;
    private Spinner mEventMethodSpinner;
    private Spinner mEventVer110MethodSpinner;
    private Switch mUseIPv6Switch;
    private WifiMonitoringThread mWifiMonitoringThread;
    private Handler mHandler;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mHandler = new Handler();

        setContentView(R.layout.activity_main);
        mEventMethodSpinner = findViewById(R.id.EventSpinner);
        mEventMethodSpinner.setSelection(1); // polling(continue)
        mEventVer110MethodSpinner = findViewById(R.id.EventVer110Spinner);
        mEventVer110MethodSpinner.setSelection(1); // polling(timeout=long)
        mUseIPv6Switch = findViewById(R.id.IPv6Switch);
        mUseIPv6Switch.setChecked(false);

        findViewById(R.id.DiscoveryButton).setOnClickListener(this);
        findViewById(R.id.DisconnectButton).setOnClickListener(this);
        findViewById(R.id.ToRemoteCaptureButton).setOnClickListener(this);
        findViewById(R.id.ToDeviceInformationButton).setOnClickListener(this);
        findViewById(R.id.ToCameraSettingButton).setOnClickListener(this);
        findViewById(R.id.ToContentsViewerButton).setOnClickListener(this);
        findViewById(R.id.ToNetworkSettingButton).setOnClickListener(this);
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy");

        if(mWifiMonitoringThread != null) {
            mWifiMonitoringThread.interrupt();
        }
    }

    @Override
    public void onClick(View v){
        if (v != null) {
            switch (v.getId()) {
                case R.id.DiscoveryButton: {
                    Button discovery = findViewById(R.id.DiscoveryButton);
                    discovery.setEnabled(false);
                    mUseIPv6Switch.setEnabled(false);

                    boolean isUseIPv6 = mUseIPv6Switch.isChecked();
                    WifiConnection wifiConnection = new WifiConnection(isUseIPv6);
                    asyncConnect(wifiConnection);
                    break;
                }
                case R.id.DisconnectButton: {
                    WebAPI.getInstance().setListener(this, this);
                    Constants.RequestCode requestcode;
                    Bundle args = new Bundle();
                    APIDataSet api = WebAPI.getInstance().getAPIData(FUNCTIONS_NETWORKCONNECTION);
                    if(api == null) {
                        args = null;
                        requestcode = Constants.RequestCode.POST_FUNCTIONS_WIFICONNECTION;
                    }else {
                        args.putString(Constants.RequestCode.POST_FUNCTIONS_NETWORKCONNECTION.name(), "disconnect");
                        requestcode = Constants.RequestCode.POST_FUNCTIONS_NETWORKCONNECTION;
                    }
                    WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(requestcode, args, new WebAPIResultListener() {
                        @Override
                        public void onWebAPIResult(WebAPIResultDataSet result) {
                            Context context = getApplicationContext();
                            if (context != null) {
                                if (result.isError()) {
                                    Toast.makeText(context, result.getErrorMsg(), Toast.LENGTH_SHORT).show();
                                }
                                else{
                                    Toast.makeText(context, "Disconnect Accepted.", Toast.LENGTH_SHORT).show();
                                }
                            }
                        }
                    }));
                    break;
                }
                case R.id.ToRemoteCaptureButton: {
                    moveToSubActivity(SubActivity.Screen.RemoteCapture);
                    break;
                }
                case R.id.ToDeviceInformationButton: {
                    moveToSubActivity(SubActivity.Screen.DeviceInformation);
                    break;
                }
                case R.id.ToCameraSettingButton: {
                    moveToSubActivity(SubActivity.Screen.CameraFunctions);
                    break;
                }
                case R.id.ToContentsViewerButton: {
                    moveToSubActivity(SubActivity.Screen.ContentsViewer);
                    break;
                }
                case R.id.ToNetworkSettingButton: {
                    moveToSubActivity(SubActivity.Screen.NetworkSetting);
                    break;
                }
                default:
                    break;
            }
        }
    }

    private void setConnectionState(boolean isConnect, String message){
        Button discovery = findViewById(R.id.DiscoveryButton);
        Button disconnect = findViewById(R.id.DisconnectButton);

        if ( isConnect ) {
            discovery.setVisibility(View.GONE);
            disconnect.setVisibility(View.VISIBLE);
        }
        else{
            discovery.setVisibility(View.VISIBLE);
            disconnect.setVisibility(View.GONE);
        }

        mUseIPv6Switch.setEnabled(!isConnect);

        APIDataSet api = WebAPI.getInstance().getAPIData(EVENT_POLLING);
        if(api != null) {
            if((api.getUrl()).contains(VER110)){
                mEventMethodSpinner.setVisibility(View.GONE);
                mEventVer110MethodSpinner.setVisibility(View.VISIBLE);
            }else{
                mEventMethodSpinner.setVisibility(View.VISIBLE);
                mEventVer110MethodSpinner.setVisibility(View.GONE);
            }
        }

        findViewById(R.id.ToRemoteCaptureButton).setEnabled(isConnect);
        findViewById(R.id.ToDeviceInformationButton).setEnabled(isConnect);
        findViewById(R.id.ToCameraSettingButton).setEnabled(isConnect);
        findViewById(R.id.ToContentsViewerButton).setEnabled(isConnect);
        /* Do not enable models that do not support the API of /networksetting */
        api = WebAPI.getInstance().getAPIData(FUNCTIONS_NETWORKSETTING);
        if(api != null){
            findViewById(R.id.ToNetworkSettingButton).setEnabled(isConnect);
        }

        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    private void asyncConnect(final WifiConnection wifiConnection){
        AsyncTask<Void, Void, String> asyncTask = new AsyncTask<Void, Void, String>(){
            @Override
            protected String doInBackground(Void... params) {
                return wifiConnection.execute();
            }

            @Override
            protected void onPostExecute(String result){
                callbackResult(result);
            }
        };
        asyncTask.execute();
    }

    public void callbackResult(String url){
        Button discovery = findViewById(R.id.DiscoveryButton);
        discovery.setEnabled(true);

        if(url != null) {
            Log.d(TAG, "Connect Success.");
            Log.d(TAG, url);

            WebAPI.getInstance().start(url);
            WebAPI.getInstance().setListener(this, this);

            // Get all APIs.
            // Generate the API list and update the screen, if they are got.
            Bundle args = new Bundle();
            String[] params = new String[]{GET, url, null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
            WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
        }
        else{
            mUseIPv6Switch.setEnabled(true);

            Toast.makeText(this, "Connect Failed.", Toast.LENGTH_SHORT).show();
            Log.d(TAG, "Connect Failed.");
        }
    }

    private void moveToSubActivity(SubActivity.Screen screen){

        String event;
        if(mEventMethodSpinner.getVisibility()==View.VISIBLE){
            event = mEventMethodSpinner.getSelectedItem().toString();
        }else{
            event = mEventVer110MethodSpinner.getSelectedItem().toString();
        }

        Constants.EventMethod method;

        switch (event) {
            case "monitoring":
                method = Constants.EventMethod.MONITORING;
                break;
            case "polling(continue=on)":
                method = Constants.EventMethod.POLLING_CONTINUE;
                break;
            case "polling(timeout=short)":
                method = Constants.EventMethod.POLLING_SHORT;
                break;
            case "polling(timeout=long)":
                method = Constants.EventMethod.POLLING_LONG;
                break;
            case "polling(timeout=immediately)":
                method = Constants.EventMethod.POLLING_IMMEDIATELY;
                break;
            case "polling":
            default:
                method = Constants.EventMethod.POLLING;
                break;
        }

        // End the monitoring of the Wi-Fi connection.
        if(mWifiMonitoringThread != null) {
            mWifiMonitoringThread.interrupt();
        }

        Intent intent = new Intent(this, SubActivity.class);
        intent.putExtra(SubActivity.SCREEN, screen);
        intent.putExtra(SubActivity.EVENT_METHOD, method);
        intent.putExtra(SubActivity.IS_USE_IPV6, mUseIPv6Switch.isChecked());
        startActivityForResult(intent, REQUEST_CODE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.d(TAG, "onActivityResult");
        switch (requestCode) {
            case (REQUEST_CODE):
                // When returning from the SubActivity by the Wi-Fi disconnection.
                if (resultCode == RESULT_OK) {
                    Bundle bundle = data.getExtras();
                    String message = bundle.getString(SubActivity.MESSAGE);
                    boolean isDisconnection = bundle.getBoolean(SubActivity.IS_DISCONNECTION);

                    if (message != null) {
                        onNotifyDisconnect(message, isDisconnection);
                    }
                }
                // When returning from the SubActivity using the return key.
                else if(resultCode == RESULT_CANCELED){
                    startWifiMonitoringThread();
                }
                break;
            default:
                break;
        }
    }

    @Override
    public void onWebAPIResult(WebAPIResultDataSet result) {
        if(!result.isError()){
            switch (result.getRequestCode()) {
                case ACT_WEB_API: {
                    //Determines whether or not a list of APIs has been obtained.
                    if(WebAPI.getInstance().isNoListOfApis(result.getResponseBody())){
                        //Use the developer API (topurlfordev) to get a list of APIs.
                        Bundle args = new Bundle();
                        String[] params = new String[]{GET, WebAPI.getInstance().getUrlForDev(), null};
                        args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
                        WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
                        break;
                    }

                    WebAPI.getInstance().setAPIDataList(result.getResponseBody());
                    APIDataSet api = WebAPI.getInstance().getAPIData(FUNCTIONS_NETWORKSETTING);
                    if(api != null){
                        Bundle args = new Bundle();
                        String[] params = new String[]{GET, api.getUrl(), null};
                        args.putStringArray(Constants.RequestCode.GET_FUNCTIONS_NETWORKSETTING.name(), params);
                        WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_NETWORKSETTING, args, this));
                    }
                    setConnectionState(true, "Connect Success.");
                    startWifiMonitoringThread();
                    break;
                }
                case GET_FUNCTIONS_NETWORKSETTING:
                    WebAPI.getInstance().setNetworkSettingAPIDataList(result.getResponseBody());
                    break;
            }
        }
        else{
            Toast.makeText(this, "Connect Failed.\n" + result.getErrorMsg(), Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void showAuthDialog(DialogFragment dialogFragment, DialogInterface.OnDismissListener onDismissListener) {
        FragmentManager fragmentManager = getSupportFragmentManager();
        dialogFragment.show(fragmentManager, "Authentication Required");
        fragmentManager.executePendingTransactions();
        dialogFragment.getDialog().setOnDismissListener(onDismissListener);
    }

    @Override
    public void onNotifyDisconnect(final String message, final boolean isDisconnection) {
        Log.d(TAG, "onNotifyDisconnect");
        if(isDisconnection) {
            // Clear digest authentication information when the wireless connection disconnected.
            WebAPI.getInstance().clearDigestAuthInfo();
        }
        else{
            startWifiMonitoringThread();
        }

        mHandler.post(new Runnable() {
            @Override
            public void run() {
                setConnectionState(!isDisconnection, message);
            }
        });
    }

    void startWifiMonitoringThread(){
        try {
            URL url = new URL(WebAPI.getInstance().getUrl());
            mWifiMonitoringThread = new WifiMonitoringThread(url.getHost(), this);
            mWifiMonitoringThread.start();
        }
        catch (MalformedURLException e) {
            e.printStackTrace();
            Log.d(TAG, "WifiMonitoringThread cannot start.");
        }
    }
}
