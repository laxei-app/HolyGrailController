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
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Toast;

import static com.canon.ccapisample.Constants.CCAPI.Field.SENSORCLEANING;

public class SensorCleaningDialogFragment extends DialogFragment{
    private static final String TAG = SensorCleaningDialogFragment.class.getSimpleName();

    private WebAPI mWebAPI;
    private CheckBox mSensorCleaningCheckBox;

    public SensorCleaningDialogFragment() {
        // Required empty public constructor
    }

    public static SensorCleaningDialogFragment newInstance(Fragment target) {
        SensorCleaningDialogFragment fragment = new SensorCleaningDialogFragment();
        fragment.setTargetFragment(target, 0);
        return fragment;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        AlertDialog.Builder dialog = new AlertDialog.Builder(getActivity());
        mWebAPI = WebAPI.getInstance();

        View view = getActivity().getLayoutInflater().inflate(R.layout.fragment_dialog_sensorcleaning, null);

        LinearLayout layout = view.findViewById(R.id.SensorCleaning);
        mSensorCleaningCheckBox = view.findViewById(R.id.SensorCleaningCheakBox);

        dialog.setTitle(SENSORCLEANING);
        dialog.setView(layout);
        dialog.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int id) {
            }
        });
        dialog.setPositiveButton("Execute", new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface dialog, int id) {
                executeSensorCleaning();
            }
        });

        return dialog.create();
    }

    @Override
    public void onStop() {
        super.onStop();
        Log.d(TAG, "onStop");
    }

    private void executeSensorCleaning(){

        Bundle args = new Bundle();
        boolean isAutoPowerOff = mSensorCleaningCheckBox.isChecked();
        args.putBoolean(Constants.RequestCode.POST_FUNCTIONS_SENSORCLEANING.name(), isAutoPowerOff);

        // Execute the API.
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.POST_FUNCTIONS_SENSORCLEANING, args, new WebAPIResultListener() {
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
