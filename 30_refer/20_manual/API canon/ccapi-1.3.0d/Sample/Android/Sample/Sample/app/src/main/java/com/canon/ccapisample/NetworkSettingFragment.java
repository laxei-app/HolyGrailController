package com.canon.ccapisample;

import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

import static com.canon.ccapisample.Constants.CCAPI.Field.ABILITY;
import static com.canon.ccapisample.Constants.CCAPI.Field.AUTHENTICATION;
import static com.canon.ccapisample.Constants.CCAPI.Field.CHANNEL;
import static com.canon.ccapisample.Constants.CCAPI.Field.COMMFUNCTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.COMMSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Field.ENCRYPTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.FUNCTIONSETTING;
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
import static com.canon.ccapisample.Constants.CCAPI.Field.MODE;
import static com.canon.ccapisample.Constants.CCAPI.Field.NW;
import static com.canon.ccapisample.Constants.CCAPI.Field.PASSWORD;
import static com.canon.ccapisample.Constants.CCAPI.Field.SSID;
import static com.canon.ccapisample.Constants.CCAPI.Field.VALUE;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_COMMSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_CONNECTSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Key.FUNCTIONS_FUNCTIONSETTING;
import static com.canon.ccapisample.Constants.CCAPI.Value.EDIT;
import static com.canon.ccapisample.Constants.CCAPI.Value.SET;

/**
 * A simple {@link Fragment} subclass.
 * Use the {@link NetworkSettingFragment#newInstance} factory method to
 * create an instance of this fragment.
 */
public class NetworkSettingFragment extends Fragment implements WebAPIResultListener, View.OnClickListener, AdapterView.OnItemSelectedListener {
    private static final String TAG = NetworkSettingFragment.class.getSimpleName();

    private static final String CONNECTION_SETTING_LIST[] = {COMMSETTING,FUNCTIONSETTING};
    private static final String COMM_SETTING_LIST[] = {LANTYPE,IPV4_IPADDRESSSET,IPV4_IPADDRESS,IPV4_SUBNETMASK,IPV4_GATEWAY,IPV6_USEIPV6,IPV6_MANUAL_SETTING,IPV6_MANUAL_ADDRESS,IPV6_PREFIXLENGTH,IPV6_GATEWAY,
                                                SSID,METHOD,CHANNEL,AUTHENTICATION,ENCRYPTION,KEYINDEX,PASSWORD};
    private static final String FUNCTION_SETTING_LIST[] = {COMMFUNCTION};

    private static final int EDIT_BUTTON_ID = 0x1;

    private WebAPI mWebAPI;

    private List<ConnectionSettingDataSet> mConnectionSettingDataSetList = new ArrayList<>();
    private List<CommSettingDataSet> mCommSettingDataSetList = new ArrayList<>();
    private List<FunctionSettingDataSet> mFunctionSettingDataSetList = new ArrayList<>();

    private Button mRebootButton;
    private Button mConnectionSettingShowButton;
    private Button mConnectionSettingCloseButton;
    private Button mCommSettingShowButton;
    private Button mCommSettingCloseButton;
    private Button mFunctionSettingShowButton;
    private Button mFunctionSettingCloseButton;

    private Spinner mCurrentConnectionSpinner;

    private ArrayAdapter<String> mConnectionAdapter;

    private Boolean mIsFromUserSelection = false;

    private LinearLayout mConnectionParentLayout;
    private LinearLayout mConnectionChildLayout;
    private LinearLayout mCommParentLayout;
    private LinearLayout mCommChildLayout;
    private LinearLayout mFunctionParentLayout;
    private LinearLayout mFunctionChildLayout;
    private LinearLayout mConnectionSettingParentLayout;
    private LinearLayout mConnectionSettingChildLayout;
    private LinearLayout mCommSettingParentLayout;
    private LinearLayout mCommSettingChildLayout;
    private LinearLayout mFunctionSettingParentLayout;
    private LinearLayout mFunctionSettingChildLayout;

    public NetworkSettingFragment() {
        // Required empty public constructor
    }

    public static NetworkSettingFragment newInstance() {
        return  new NetworkSettingFragment();
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mWebAPI = WebAPI.getInstance();
        mConnectionAdapter = new ArrayAdapter<>(getActivity(), android.R.layout.simple_spinner_item);
        // Get API list for network settings.
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_CURRENTCONNECT_SETTING, null, this));
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_CONNECT_SETTING, null, this));
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_COMM_SETTING, null, this));
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_FUNCTION_SETTING, null, this));
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container,
                             Bundle savedInstanceState) {
        Log.d(TAG, "onCreateView");

        View view = inflater.inflate(R.layout.fragment_network_setting, container, false);

        mCurrentConnectionSpinner = view.findViewById(R.id.CurrentConnectSpinner);

        // OnClickListener
        mRebootButton = view.findViewById(R.id.RebootButton);
        mRebootButton.setOnClickListener(this);
        mConnectionSettingShowButton = view.findViewById(R.id.ConnectionSettingShowButton);
        mConnectionSettingShowButton.setOnClickListener(this);
        mConnectionSettingCloseButton = view.findViewById(R.id.ConnectionSettingCloseButton);
        mConnectionSettingCloseButton.setOnClickListener(this);
        mCommSettingShowButton = view.findViewById(R.id.CommSettingShowButton);
        mCommSettingShowButton.setOnClickListener(this);
        mCommSettingCloseButton = view.findViewById(R.id.CommSettingCloseButton);
        mCommSettingCloseButton.setOnClickListener(this);
        mFunctionSettingShowButton = view.findViewById(R.id.FunctionSettingShowButton);
        mFunctionSettingShowButton.setOnClickListener(this);
        mFunctionSettingCloseButton = view.findViewById(R.id.FunctionSettingCloseButton);
        mFunctionSettingCloseButton.setOnClickListener(this);
        view.findViewById(R.id.CurrentConnectSetButton).setOnClickListener(this);
        view.findViewById(R.id.CurrentConnectUpdateButton).setOnClickListener(this);
        view.findViewById(R.id.ConnectionButton).setOnClickListener(this);
        view.findViewById(R.id.CommButton).setOnClickListener(this);
        view.findViewById(R.id.FunctionButton).setOnClickListener(this);

        mCurrentConnectionSpinner.setAdapter(mConnectionAdapter);

        View.OnTouchListener onTouchListener = new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                Log.d(TAG, "Spinner onTouchListener");
                mIsFromUserSelection = true;
                return false;
            }
        };
        mCurrentConnectionSpinner.setOnTouchListener(onTouchListener);

        // OnItemSelectedListener
        mCurrentConnectionSpinner.setOnItemSelectedListener(this);

        // Network Setting
        mConnectionParentLayout = view.findViewById(R.id.ConnectionParentLayout);
        mCommParentLayout = view.findViewById(R.id.CommParentLayout);
        mFunctionParentLayout = view.findViewById(R.id.FunctionParentLayout);

        mConnectionChildLayout = view.findViewById(R.id.ConnectionChildLayout);
        mCommChildLayout = view.findViewById(R.id.CommChildLayout);
        mFunctionChildLayout = view.findViewById(R.id.FunctionChildLayout);

        mConnectionSettingParentLayout = view.findViewById(R.id.ConnectionSettingParentLayout);
        mCommSettingParentLayout = view.findViewById(R.id.CommSettingParentLayout);
        mFunctionSettingParentLayout = view.findViewById(R.id.FunctionSettingParentLayout);

        mConnectionSettingChildLayout = view.findViewById(R.id.ConnectionSettingChildLayout);
        mCommSettingChildLayout = view.findViewById(R.id.CommSettingChildLayout);
        mFunctionSettingChildLayout = view.findViewById(R.id.FunctionSettingChildLayout);

        return view;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy");
    }

    @Override
    public void onClick(View v){
        if (v != null) {
            switch (v.getId()) {
                case R.id.ConnectionButton:{
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_CONNECT_SETTING, null, this));
                    // Change the display.
                    mConnectionParentLayout.setVisibility(View.VISIBLE);
                    mCommParentLayout.setVisibility(View.GONE);
                    mFunctionParentLayout.setVisibility(View.GONE);

                    mConnectionSettingParentLayout.setVisibility(View.GONE);
                    mCommSettingParentLayout.setVisibility(View.GONE);
                    mFunctionSettingParentLayout.setVisibility(View.GONE);

                    break;
                }
                case R.id.CommButton:{
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_COMM_SETTING, null, this));
                    // Change the display.
                    mConnectionParentLayout.setVisibility(View.GONE);
                    mCommParentLayout.setVisibility(View.VISIBLE);
                    mFunctionParentLayout.setVisibility(View.GONE);

                    mConnectionSettingParentLayout.setVisibility(View.GONE);
                    mCommSettingParentLayout.setVisibility(View.GONE);
                    mFunctionSettingParentLayout.setVisibility(View.GONE);

                    break;
                }
                case R.id.FunctionButton:{
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_FUNCTION_SETTING, null, this));
                    // Change the display.
                    mConnectionParentLayout.setVisibility(View.GONE);
                    mCommParentLayout.setVisibility(View.GONE);
                    mFunctionParentLayout.setVisibility(View.VISIBLE);

                    mConnectionSettingParentLayout.setVisibility(View.GONE);
                    mCommSettingParentLayout.setVisibility(View.GONE);
                    mFunctionSettingParentLayout.setVisibility(View.GONE);

                    break;
                }
                case R.id.RebootButton: {
                    Bundle args = new Bundle();
                    args.putString(Constants.RequestCode.POST_FUNCTIONS_NETWORKCONNECTION.name(), "reboot");
                    WebAPI.getInstance().enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.POST_FUNCTIONS_NETWORKCONNECTION, args, this));
                    break;
                }
                case R.id.CurrentConnectSetButton: {
                    Bundle args = new Bundle();
                    String value = (String) mCurrentConnectionSpinner.getSelectedItem();
                    args.putString(Constants.RequestCode.PUT_FUNCTIONS_CURRENTCONNECT_SETTING.name(), value);
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.PUT_FUNCTIONS_CURRENTCONNECT_SETTING, args, this));
                    break;
                }
                case R.id.CurrentConnectUpdateButton: {
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_CURRENTCONNECT_SETTING, null, this));
                    break;
                }
                case R.id.ConnectionSettingShowButton: {
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_CONNECT_SETTING, null, this));
                    mConnectionParentLayout.setVisibility(View.GONE);
                    mConnectionSettingParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case R.id.ConnectionSettingCloseButton: {
                    mConnectionSettingParentLayout.setVisibility(View.GONE);
                    mConnectionParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case R.id.CommSettingShowButton: {
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_COMM_SETTING, null, this));
                    mCommParentLayout.setVisibility(View.GONE);
                    mCommSettingParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case R.id.CommSettingCloseButton: {
                    mCommSettingParentLayout.setVisibility(View.GONE);
                    mCommParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case R.id.FunctionSettingShowButton: {
                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_FUNCTIONS_FUNCTION_SETTING, null, this));
                    mFunctionParentLayout.setVisibility(View.GONE);
                    mFunctionSettingParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case R.id.FunctionSettingCloseButton: {
                    mFunctionSettingParentLayout.setVisibility(View.GONE);
                    mFunctionParentLayout.setVisibility(View.VISIBLE);
                    break;
                }
                case EDIT_BUTTON_ID: {
                   String tag = v.getTag().toString();
                    if(tag.contains(SET)){
                        NetworkConnectionSettingDialogFragment dialog = NetworkConnectionSettingDialogFragment.newInstance(this, (FUNCTIONS_CONNECTSETTING + "/" + tag));
                        dialog.show(getActivity().getSupportFragmentManager(), tag);
                    }else if(tag.contains(NW)){
                        NetworkCommSettingDialogFragment dialog = NetworkCommSettingDialogFragment.newInstance(this, (FUNCTIONS_COMMSETTING + "/" + tag));
                        dialog.show(getActivity().getSupportFragmentManager(), tag);
                    }else if(tag.contains(MODE)){
                        NetworkFunctionSettingDialogFragment dialog = NetworkFunctionSettingDialogFragment.newInstance(this, (FUNCTIONS_FUNCTIONSETTING + "/" + tag));
                        dialog.show(getActivity().getSupportFragmentManager(), tag);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    /**
     * Callback from execute WebAPI
     * @param result HTTP Request result
     */
    @Override
    public void onWebAPIResult(WebAPIResultDataSet result) {
        Log.d(TAG, String.format("%s onWebAPIResult", String.valueOf(result.getRequestCode())));
        Context context = getActivity();

        // Do nothing, if life cycle of the fragment is finished.
        if (context != null) {
            if (result.isError()) {
                Toast.makeText(context, result.getErrorMsg(), Toast.LENGTH_SHORT).show();
            }
            else {
                switch (result.getRequestCode()) {
                    case GET_FUNCTIONS_CURRENTCONNECT_SETTING: {

                        // The callback from information acquisition of the current connection setting on the onCreateView().
                        mConnectionAdapter.clear();
                        try {
                            JSONObject jsonObject = new JSONObject(result.getResponseBody());
                            String currentvalue = jsonObject.get( VALUE ).toString();
                            JSONArray currentconnectArray = jsonObject.getJSONArray(ABILITY);

                            for (int i = 0; i < currentconnectArray.length(); i++) {
                                String name = currentconnectArray.get(i).toString();
                                mConnectionAdapter.add(name);
                            }

                            mConnectionAdapter.notifyDataSetChanged();
                            mCurrentConnectionSpinner.setSelection(mConnectionAdapter.getPosition(currentvalue));

                        }
                        catch (JSONException e) {
                            e.printStackTrace();
                        }
                        break;
                    }
                    case GET_FUNCTIONS_CONNECT_SETTING: {
                        mConnectionSettingDataSetList.clear();
                        try {
                            JSONObject jsonObject = new JSONObject(result.getResponseBody());
                            String set = "";
                            for(int i = 0; i < jsonObject.length(); i++) {
                                set = SET + String.format("%02d", (i+1));
                                ConnectionSettingDataSet ConnectionSettingDataSet = new ConnectionSettingDataSet(set, jsonObject.getJSONObject(set));
                                mConnectionSettingDataSetList.add(ConnectionSettingDataSet);
                            }
                        } catch (JSONException e) {
                            e.printStackTrace();
                        }

                        mConnectionChildLayout.removeAllViews();

                        for(int i=0;i<mConnectionSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.HORIZONTAL);
                            String name = mConnectionSettingDataSetList.get(i).getSet();

                            LinearLayout.LayoutParams TextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            TextLayoutParams.weight = 2;

                            TextView textView = new TextView(getContext());
                            textView.setText(name);
                            textView.setId(i);
                            setLayout.addView(textView,TextLayoutParams);

                            LinearLayout.LayoutParams ButtonLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            ButtonLayoutParams.weight = 1;

                            Button button = new Button(getContext());

                            button.setText(EDIT);
                            button.setId(EDIT_BUTTON_ID);
                            button.setTag(name);
                            button.setOnClickListener(this);
                            setLayout.addView(button,ButtonLayoutParams);

                            mConnectionChildLayout.addView(setLayout);
                        }

                        mConnectionSettingChildLayout.removeAllViews();

                        for(int i=0;i<mConnectionSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.VERTICAL);

                            String set = mConnectionSettingDataSetList.get(i).getSet();

                            String nw1 = mConnectionSettingDataSetList.get(i).getNW1();
                            String nw2 = mConnectionSettingDataSetList.get(i).getNW2();
                            String mode1 = mConnectionSettingDataSetList.get(i).getMode1();
                            String mode2 = mConnectionSettingDataSetList.get(i).getMode2();

                            String Connection_Field[][] = {{nw1,nw2},{mode1,mode2}};

                            LinearLayout.LayoutParams SetTextLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            SetTextLayoutParams.weight = 2;

                            LinearLayout.LayoutParams IndexTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            IndexTextLayoutParams.weight = 2;

                            /* index */
                            LinearLayout indexLayout = new LinearLayout(getContext());
                            indexLayout.setOrientation(LinearLayout.HORIZONTAL);

                            TextView SetTextView = new TextView(getContext());
                            SetTextView.setText(set);
                            SetTextView.setId(i);
                            SetTextView.setBackgroundColor(Color.LTGRAY);
                            SetTextView.setTextColor(Color.WHITE);
                            indexLayout.addView(SetTextView,IndexTextLayoutParams);

                            setLayout.addView(indexLayout,SetTextLayoutParams);

                            /* setting */
                            LinearLayout settingLayout = new LinearLayout(getContext());
                            settingLayout.setOrientation(LinearLayout.VERTICAL);
                            settingLayout.setPadding(0,0,0,20);

                            LinearLayout.LayoutParams SettingLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            SettingLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldNameTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldNameTextLayoutParams.weight = 2;

                            LinearLayout.LayoutParams FieldTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldTextLayoutParams.weight = 2;

                            for(int j = 0; j < CONNECTION_SETTING_LIST.length; j++) {
                                /* field name */
                                LinearLayout fieldLayout = new LinearLayout(getContext());
                                fieldLayout.setOrientation(LinearLayout.HORIZONTAL);

                                TextView FieldNameTextView = new TextView(getContext());
                                FieldNameTextView.setText(CONNECTION_SETTING_LIST[j]);
                                FieldNameTextView.setPadding(0,20,0,0);
                                FieldNameTextView.setId(i);
                                fieldLayout.addView(FieldNameTextView,FieldNameTextLayoutParams);

                                for(int k = 0; k < Connection_Field[j].length; k++) {
                                    TextView FieldTextView = new TextView(getContext());
                                    FieldTextView.setText(Connection_Field[j][k]);
                                    FieldTextView.setPadding(0,20,0,0);
                                    FieldTextView.setGravity(Gravity.CENTER);
                                    FieldTextView.setId(i);
                                    fieldLayout.addView(FieldTextView, FieldNameTextLayoutParams);
                                }

                                settingLayout.addView(fieldLayout,FieldLayoutParams);
                            }

                            setLayout.addView(settingLayout,SettingLayoutParams);

                            mConnectionSettingChildLayout.addView(setLayout);
                        }

                        break;
                    }
                    case GET_FUNCTIONS_COMM_SETTING: {
                        mCommSettingDataSetList.clear();
                        try {
                            JSONObject jsonObject = new JSONObject(result.getResponseBody());
                            String nw = "";
                            for(int i = 0; i < jsonObject.length(); i++) {
                                nw = NW + String.format("%02d", (i+1));
                                CommSettingDataSet CommSettingDataSet = new CommSettingDataSet(nw, jsonObject.getJSONObject(nw));
                                mCommSettingDataSetList.add(CommSettingDataSet);
                            }
                        } catch (JSONException e) {
                            e.printStackTrace();
                        }

                        mCommChildLayout.removeAllViews();

                        for(int i=0;i<mCommSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.HORIZONTAL);
                            String name = mCommSettingDataSetList.get(i).getNW();

                            LinearLayout.LayoutParams TextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            TextLayoutParams.weight = 2;

                            TextView textView = new TextView(getContext());
                            textView.setText(name);
                            textView.setId(i);
                            setLayout.addView(textView,TextLayoutParams);

                            LinearLayout.LayoutParams ButtonLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            ButtonLayoutParams.weight = 1;

                            Button button = new Button(getContext());

                            button.setText(EDIT);
                            button.setId(EDIT_BUTTON_ID);
                            button.setTag(name);
                            button.setOnClickListener(this);
                            setLayout.addView(button,ButtonLayoutParams);

                            mCommChildLayout.addView(setLayout);
                        }

                        mCommSettingChildLayout.removeAllViews();

                        for(int i=0;i<mCommSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.VERTICAL);

                            String name = mCommSettingDataSetList.get(i).getNW();

                            String lantype = mCommSettingDataSetList.get(i).getLantype();
                            String ipv4_ipaddressset = mCommSettingDataSetList.get(i).getIpv4_ipaddressset();
                            String ipv4_ipaddress = mCommSettingDataSetList.get(i).getIpv4_ipaddress();
                            String ipv4_subnetmask = mCommSettingDataSetList.get(i).getIpv4_subnetmask();
                            String ipv4_gateway = mCommSettingDataSetList.get(i).getIpv4_gateway();
                            String ipv6_useipv6 = mCommSettingDataSetList.get(i).getIpv6_useipv6();
                            String ipv6_manual_setting = mCommSettingDataSetList.get(i).getIpv6_manual_setting();
                            String ipv6_manual_address = mCommSettingDataSetList.get(i).getIpv6_manual_address();
                            String ipv6_prefixlength = mCommSettingDataSetList.get(i).getIpv6_prefixlength();
                            String ipv6_gateway = mCommSettingDataSetList.get(i).getIpv6_gateway();
                            String ssid = mCommSettingDataSetList.get(i).getSsid();
                            String method = mCommSettingDataSetList.get(i).getMethod();
                            String channel = mCommSettingDataSetList.get(i).getChannel();
                            String authentication = mCommSettingDataSetList.get(i).getAuthentication();
                            String encryption = mCommSettingDataSetList.get(i).getEncryption();
                            String keyindex = mCommSettingDataSetList.get(i).getKeyindex();
                            String password = mCommSettingDataSetList.get(i).getPassword();

                            String Comm_Field[] = {lantype,ipv4_ipaddressset,ipv4_ipaddress,ipv4_subnetmask,ipv4_gateway,ipv6_useipv6,ipv6_manual_setting,ipv6_manual_address,ipv6_prefixlength,ipv6_gateway,
                                                    ssid,method,channel,authentication,encryption,keyindex,password};

                            LinearLayout.LayoutParams NWTextLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            NWTextLayoutParams.weight = 2;

                            /* index */
                            TextView NWTextView = new TextView(getContext());
                            NWTextView.setText(name);
                            NWTextView.setId(i);
                            NWTextView.setBackgroundColor(Color.LTGRAY);
                            NWTextView.setTextColor(Color.WHITE);
                            setLayout.addView(NWTextView,NWTextLayoutParams);

                            /* setting */
                            LinearLayout settingLayout = new LinearLayout(getContext());
                            settingLayout.setOrientation(LinearLayout.VERTICAL);
                            settingLayout.setPadding(0,20,0,20);

                            LinearLayout.LayoutParams SettingLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            SettingLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldNameTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldNameTextLayoutParams.weight = 2;

                            LinearLayout.LayoutParams FieldTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldTextLayoutParams.weight = 2;

                            for(int j = 0; j < COMM_SETTING_LIST.length; j++) {
                                /* field name */
                                LinearLayout fieldLayout = new LinearLayout(getContext());
                                fieldLayout.setOrientation(LinearLayout.HORIZONTAL);

                                TextView FieldNameTextView = new TextView(getContext());
                                FieldNameTextView.setText(COMM_SETTING_LIST[j]);
                                FieldNameTextView.setId(i);
                                fieldLayout.addView(FieldNameTextView,FieldNameTextLayoutParams);

                                TextView FieldTextView = new TextView(getContext());
                                FieldTextView.setText(Comm_Field[j]);
                                FieldTextView.setGravity(Gravity.CENTER);
                                FieldTextView.setId(i);
                                fieldLayout.addView(FieldTextView,FieldNameTextLayoutParams);

                                settingLayout.addView(fieldLayout,FieldLayoutParams);
                            }

                            setLayout.addView(settingLayout,SettingLayoutParams);

                            mCommSettingChildLayout.addView(setLayout);
                        }
                        break;
                    }
                    case GET_FUNCTIONS_FUNCTION_SETTING: {
                        mFunctionSettingDataSetList.clear();
                        try {
                            JSONObject jsonObject = new JSONObject(result.getResponseBody());
                            String mode = "";
                            for(int i = 0; i < jsonObject.length(); i++) {
                                mode = MODE + String.format("%02d", (i+1));
                                FunctionSettingDataSet FunctionSettingDataSet = new FunctionSettingDataSet(mode, jsonObject.getJSONObject(mode));
                                mFunctionSettingDataSetList.add(FunctionSettingDataSet);
                            }
                        } catch (JSONException e) {
                            e.printStackTrace();
                        }

                        mFunctionChildLayout.removeAllViews();

                        for(int i=0;i<mFunctionSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.HORIZONTAL);
                            String name = mFunctionSettingDataSetList.get(i).getMode();

                            LinearLayout.LayoutParams TextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            TextLayoutParams.weight = 2;

                            TextView textView = new TextView(getContext());
                            textView.setText(name);
                            textView.setId(i);
                            setLayout.addView(textView,TextLayoutParams);

                            LinearLayout.LayoutParams ButtonLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.WRAP_CONTENT);
                            ButtonLayoutParams.weight = 1;

                            Button button = new Button(getContext());

                            button.setText(EDIT);
                            button.setId(EDIT_BUTTON_ID);
                            button.setTag(name);
                            button.setOnClickListener(this);
                            setLayout.addView(button,ButtonLayoutParams);

                            mFunctionChildLayout.addView(setLayout);
                        }

                        mFunctionSettingChildLayout.removeAllViews();

                        for(int i=0;i<mFunctionSettingDataSetList.size();i++) {

                            LinearLayout setLayout = new LinearLayout(getContext());
                            setLayout.setOrientation(LinearLayout.VERTICAL);

                            String mode = mFunctionSettingDataSetList.get(i).getMode();

                            String commfunction = mFunctionSettingDataSetList.get(i).getCommfunction();

                            String CommFunction_Field[] = {commfunction};

                            LinearLayout.LayoutParams ModeTextLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            ModeTextLayoutParams.weight = 2;

                            /* index */
                            TextView ModeTextView = new TextView(getContext());
                            ModeTextView.setText(mode);
                            ModeTextView.setId(i);
                            ModeTextView.setBackgroundColor(Color.LTGRAY);
                            ModeTextView.setTextColor(Color.WHITE);
                            setLayout.addView(ModeTextView,ModeTextLayoutParams);

                            /* setting */
                            LinearLayout settingLayout = new LinearLayout(getContext());
                            settingLayout.setOrientation(LinearLayout.VERTICAL);

                            LinearLayout.LayoutParams SettingLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            SettingLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldLayoutParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldLayoutParams.weight = 1;

                            LinearLayout.LayoutParams FieldNameTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldNameTextLayoutParams.weight = 2;

                            LinearLayout.LayoutParams FieldTextLayoutParams = new LinearLayout.LayoutParams(0,ViewGroup.LayoutParams.MATCH_PARENT);
                            FieldTextLayoutParams.weight = 2;

                            for(int j = 0; j < FUNCTION_SETTING_LIST.length; j++) {
                                /* field name */
                                LinearLayout fieldLayout = new LinearLayout(getContext());
                                fieldLayout.setOrientation(LinearLayout.HORIZONTAL);

                                TextView FieldNameTextView = new TextView(getContext());
                                FieldNameTextView.setText(FUNCTION_SETTING_LIST[j]);
                                FieldNameTextView.setPadding(0,20,0,20);
                                FieldNameTextView.setId(i);
                                fieldLayout.addView(FieldNameTextView,FieldNameTextLayoutParams);

                                TextView FieldTextView = new TextView(getContext());
                                FieldTextView.setText(CommFunction_Field[j]);
                                FieldTextView.setGravity(Gravity.CENTER);
                                FieldTextView.setPadding(0,20,0,20);
                                FieldTextView.setId(i);
                                fieldLayout.addView(FieldTextView,FieldNameTextLayoutParams);

                                settingLayout.addView(fieldLayout,FieldLayoutParams);
                            }

                            setLayout.addView(settingLayout,SettingLayoutParams);

                            mFunctionSettingChildLayout.addView(setLayout);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        else{
            Log.d(TAG, String.format("%s Activity is Null.", String.valueOf(result.getRequestCode())));
        }
    }

    @Override
    public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
        Log.d(TAG, "onItemSelected");

        if(mIsFromUserSelection) {
            Log.d(TAG, "onItemSelected IsFromUser");
            Spinner spinner = (Spinner) parent;
            String item = (String) spinner.getSelectedItem();

            switch (parent.getId()){
                case R.id.CurrentConnectSpinner: {
                    Log.d(TAG, "onItemSelected CurrentConnectSpinner");
                }
                default:
                    break;
            }
        }

        mIsFromUserSelection = false;
    }

    @Override
    public void onNothingSelected(AdapterView<?> parent) {
        Log.d(TAG, "onNothingSelected");
        mIsFromUserSelection = false;
    }

}
