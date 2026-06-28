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
import android.widget.TextView;
import android.widget.Toast;

import static com.canon.ccapisample.Constants.CCAPI.Field.FREQUENCY;
import static com.canon.ccapisample.Constants.CCAPI.Field.TV;

public class HFFlickerTvSettingDialogFragment extends DialogFragment{
    private static final String TAG = HFFlickerTvSettingDialogFragment.class.getSimpleName();

    private WebAPI mWebAPI;
    private TextView mHFFlickerTvResultTvText;

    public HFFlickerTvSettingDialogFragment() {
        // Required empty public constructor
    }

    public static HFFlickerTvSettingDialogFragment newInstance(Fragment target, Bundle args) {
        HFFlickerTvSettingDialogFragment fragment = new HFFlickerTvSettingDialogFragment();
        fragment.setTargetFragment(target, 0);
        fragment.setArguments(args);
        return fragment;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        AlertDialog.Builder dialog = new AlertDialog.Builder(getActivity());
        mWebAPI = WebAPI.getInstance();

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_hf_flicker_tv_setting, null);

        LinearLayout layout = view.findViewById(R.id.HFFlickerTvSetting);
        TextView mHFFlickerTvResultFreqText = view.findViewById(R.id.HFFlickerTvResultFreq);
        mHFFlickerTvResultTvText = view.findViewById(R.id.HFFlickerTvResultTv);

        String Freq = getArguments().getString(FREQUENCY);
        String Tv = getArguments().getString(TV);

        mHFFlickerTvResultFreqText.setText(Freq);
        mHFFlickerTvResultTvText.setText(Tv);

        dialog.setTitle("HF Flicker Tv");
        dialog.setView(layout);
        dialog.setPositiveButton("Set", new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface dialog, int id) {
                setHFFlickerTv();
            }
        });

        return dialog.create();
    }

    @Override
    public void onStop() {
        super.onStop();
        Log.d(TAG, "onStop");
    }

    private void setHFFlickerTv(){

        Bundle args = new Bundle();
        String value = mHFFlickerTvResultTvText.getText().toString();
        args.putString(Constants.RequestCode.PUT_SHOOTING_SETTINGS_HFFLICKERTV.name(), value);

        // Execute the API.
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.PUT_SHOOTING_SETTINGS_HFFLICKERTV, args, new WebAPIResultListener() {
            @Override
            public void onWebAPIResult(WebAPIResultDataSet result) {
                Context context = getActivity();
                // Do nothing, if life cycle of the fragment is finished.
                if(context != null) {
                    if (result.isError()) {
                        Toast.makeText(context, result.getErrorMsg(), Toast.LENGTH_SHORT).show();
                    }
                }
            }
        }));
    }
}
