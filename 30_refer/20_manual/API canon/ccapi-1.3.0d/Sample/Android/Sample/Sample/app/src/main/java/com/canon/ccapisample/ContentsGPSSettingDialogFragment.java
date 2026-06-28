package com.canon.ccapisample;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.DialogInterface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.text.TextUtils;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.Spinner;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import static com.canon.ccapisample.Constants.CCAPI.Field.ACTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.AUTHENTICATION;
import static com.canon.ccapisample.Constants.CCAPI.Field.CHANNEL;
import static com.canon.ccapisample.Constants.CCAPI.Field.ENCRYPTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.GATEWAY;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPADDRESS;
import static com.canon.ccapisample.Constants.CCAPI.Field.IPADDRESSSET;
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
import static com.canon.ccapisample.Constants.CCAPI.Field.METHOD;
import static com.canon.ccapisample.Constants.CCAPI.Field.PASSWORD;
import static com.canon.ccapisample.Constants.CCAPI.Field.SSID;
import static com.canon.ccapisample.Constants.CCAPI.Field.SUBNETMASK;
import static com.canon.ccapisample.Constants.CCAPI.Field.VALUE;
import static com.canon.ccapisample.Constants.CCAPI.Method.PUT;
import static com.canon.ccapisample.Constants.CCAPI.Value.ARCHIVE;
import static com.canon.ccapisample.Constants.CCAPI.Value.GPS;
import static com.canon.ccapisample.Constants.CCAPI.Value.PROTECT;
import static com.canon.ccapisample.Constants.CCAPI.Value.RATING;
import static com.canon.ccapisample.Constants.CCAPI.Value.ROTATE;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_ARRAY;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_EMBEDDED;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_DISPLAY;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_ORIGINAL;

public class ContentsGPSSettingDialogFragment extends DialogFragment {

    private static final String GPS_HOUR = "hour";
    private static final String GPS_DEGREE = "degree";
    private static final String GPS_MINUTE = "minute";
    private static final String GPS_SECOND = "second";
    private static final String GPS_LATITUDE_REF = "latitude_ref";
    private static final String GPS_LATITUDE = "latitude";
    private static final String GPS_LONGITUDE_REF = "longitude_ref";
    private static final String GPS_LONGITUDE = "longitude";
    private static final String GPS_ALTITUDE_REF = "altitude_ref";
    private static final String GPS_ALTITUDE = "altitude";
    private static final String GPS_TIMESTAMP = "timestamp";
    private static final String GPS_MAPDATUM = "mapdatum";
    private static final String GPS_STATUS = "status";
    private static final String GPS_DATESTAMP = "datestamp";
    private static final String[] GPS_LATITUDE_REF_VALUE = {"N", "S"};
    private static final String[] GPS_LONGITUDE_REF_VALUE = {"E", "W"};
    private static final String[] GPS_ALTITUDE_REF_VALUE = {"P", "M"};
    private static final String[] GPS_STATUS_VALUE = {"A", "V"};

    private static final String TAG = ContentsGPSSettingDialogFragment.class.getSimpleName();
    private static final String STORAGE_CONTENTS_DATA_SET = "ContentsDataSet";
    private WebAPI mWebAPI;
    private ContentsDataSet mContentsDataSet;
    private Uri mSaveDirUri;

    public static ContentsGPSSettingDialogFragment newInstance(Fragment target, ContentsDataSet contentsDataSet){
        ContentsGPSSettingDialogFragment instance = new ContentsGPSSettingDialogFragment();
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

        AlertDialog.Builder dialogBuilder = new AlertDialog.Builder(getActivity());
        mWebAPI = WebAPI.getInstance();

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_gps_setting, null);

        LinearLayout layout = view.findViewById(R.id.GPSSetting);

        dialogBuilder.setTitle(mContentsDataSet.getName() + ": ACTION_GPS");
        dialogBuilder.setView(layout);
        dialogBuilder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface dialog, int id) {
                putGPSAction();
            }
        });

        return dialogBuilder.create();
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

    private void getGPSSetting(JSONObject data){

        RadioGroup radioGroup;
        int radioID;
        JSONObject valueObject = new JSONObject();
        EditText editText;
        EditText editNumeratorText;
        EditText editDenominatorText;
        String NumeratorText;
        String DenominatorText;
        int numerator;
        int denominator;
        Dialog view = getDialog();

        try {

            /* latitude_ref */

            radioGroup = (RadioGroup) view.findViewById(R.id.LatitudeRefRadio);
            radioID = radioGroup.getCheckedRadioButtonId();
            if(radioID == R.id.LatitudeRefNRadioButton){
                data.put(GPS_LATITUDE_REF, GPS_LATITUDE_REF_VALUE[0]);
            }else{
                data.put(GPS_LATITUDE_REF, GPS_LATITUDE_REF_VALUE[1]);
            }

            /* latitude */

            /* degree */
            JSONArray latitude_degree_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LatitudeDegreeNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LatitudeDegreeDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            latitude_degree_value.put(numerator);
            latitude_degree_value.put(denominator);
            valueObject.put(GPS_DEGREE, latitude_degree_value);

            /* minute */
            JSONArray latitude_minute_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LatitudeMinuteNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LatitudeMinuteDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            latitude_minute_value.put(numerator);
            latitude_minute_value.put(denominator);
            valueObject.put(GPS_MINUTE, latitude_minute_value);

            /* second */
            JSONArray latitude_second_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LatitudeSecondNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LatitudeSecondDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            latitude_second_value.put(numerator);
            latitude_second_value.put(denominator);
            valueObject.put(GPS_SECOND, latitude_second_value);

            data.accumulate(GPS_LATITUDE,valueObject);

            /* longitude_ref */

            radioGroup = (RadioGroup) view.findViewById(R.id.LongitudeRefRadio);
            radioID = radioGroup.getCheckedRadioButtonId();
            if(radioID == R.id.LongitudeRefERadioButton){
                data.put(GPS_LONGITUDE_REF, GPS_LONGITUDE_REF_VALUE[0]);
            }else{
                data.put(GPS_LONGITUDE_REF, GPS_LONGITUDE_REF_VALUE[1]);
            }

            /* longitude */

            /* degree */
            JSONArray longitude_degree_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LongitudeDegreeNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LongitudeDegreeDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            longitude_degree_value.put(numerator);
            longitude_degree_value.put(denominator);
            valueObject.put(GPS_DEGREE, longitude_degree_value);

            /* minute */
            JSONArray longitude_minute_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LongitudeMinuteNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LongitudeMinuteDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            longitude_minute_value.put(numerator);
            longitude_minute_value.put(denominator);
            valueObject.put(GPS_MINUTE, longitude_minute_value);

            /* second */
            JSONArray longitude_second_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.LongitudeSecondNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.LongitudeSecondDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            longitude_second_value.put(numerator);
            longitude_second_value.put(denominator);
            valueObject.put(GPS_SECOND, longitude_second_value);

            data.accumulate(GPS_LONGITUDE,valueObject);

            /* altitude_ref */

            radioGroup = (RadioGroup) view.findViewById(R.id.AltitudeRefRadio);
            radioID = radioGroup.getCheckedRadioButtonId();
            if(radioID == R.id.AltitudeRefPRadioButton){
                data.put(GPS_ALTITUDE_REF, GPS_ALTITUDE_REF_VALUE[0]);
            }else{
                data.put(GPS_ALTITUDE_REF, GPS_ALTITUDE_REF_VALUE[1]);
            }

            /* altitude */

            JSONArray altitude_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.AltitudeNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.AltitudeDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            altitude_value.put(numerator);
            altitude_value.put(denominator);
            data.put(GPS_ALTITUDE,altitude_value);

            /* timestamp */

            /* hour */
            JSONArray timestamp_hour_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.TimestampHourNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.TimestampHourDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            timestamp_hour_value.put(numerator);
            timestamp_hour_value.put(denominator);
            valueObject.put(GPS_HOUR, timestamp_hour_value);

            /* minute */
            JSONArray timestamp_minute_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.TimestampMinuteNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.TimestampMinuteDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            timestamp_minute_value.put(numerator);
            timestamp_minute_value.put(denominator);
            valueObject.put(GPS_MINUTE, timestamp_minute_value);

            /* second */
            JSONArray timestamp_second_value = new JSONArray();
            editNumeratorText = view.findViewById(R.id.TimestampSecondNumerator);
            NumeratorText = editNumeratorText.getText().toString();
            if(!TextUtils.isEmpty(NumeratorText)){
                numerator = Integer.parseInt(NumeratorText);
            }else{
                numerator = 0;
            }
            editDenominatorText = view.findViewById(R.id.TimestampSecondDenominator);
            DenominatorText = editDenominatorText.getText().toString();
            if(!TextUtils.isEmpty(DenominatorText)){
                denominator = Integer.parseInt(DenominatorText);
            }else{
                denominator = 0;
            }
            timestamp_second_value.put(numerator);
            timestamp_second_value.put(denominator);
            valueObject.put(GPS_SECOND, timestamp_second_value);

            data.accumulate(GPS_TIMESTAMP,valueObject);

            /* mapdatum */

            editText = view.findViewById(R.id.MapdatumEdit);
            data.put(GPS_MAPDATUM, editText.getText().toString());

            /* status */

            radioGroup = (RadioGroup) view.findViewById(R.id.StatusRadio);
            radioID = radioGroup.getCheckedRadioButtonId();
            if(radioID == R.id.StatusARadioButton){
                data.put(GPS_STATUS, GPS_STATUS_VALUE[0]);
            }else{
                data.put(GPS_STATUS, GPS_STATUS_VALUE[1]);
            }

            /* datestamp */

            editText = view.findViewById(R.id.DatestampEdit);
            data.put(GPS_DATESTAMP, editText.getText().toString());

        }
        catch (JSONException e) {
            e.printStackTrace();
        }

    }

    private void putGPSAction(){
        JSONObject body = new JSONObject();
        JSONObject data = new JSONObject();
        try {
            body.put(ACTION, GPS);
            getGPSSetting(data);
            body.accumulate(GPS,data);
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
