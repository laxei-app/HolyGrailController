package com.canon.ccapisample;

import android.app.AlertDialog;
import android.app.Dialog;
import android.os.Bundle;
import android.support.annotation.NonNull;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;

import static com.canon.ccapisample.Constants.CCAPI.Value.ARCHIVE;
import static com.canon.ccapisample.Constants.CCAPI.Value.GPS;
import static com.canon.ccapisample.Constants.CCAPI.Value.XMP;
import static com.canon.ccapisample.Constants.CCAPI.Value.PROTECT;
import static com.canon.ccapisample.Constants.CCAPI.Value.RATING;
import static com.canon.ccapisample.Constants.CCAPI.Value.ROTATE;

import java.util.ArrayList;
import java.util.List;

public class ContentsEditDialogFragment extends DialogFragment implements AdapterView.OnItemClickListener {
    private final List<String> EDIT_MENU = new ArrayList<String>(){
        {
            add(ROTATE);
            add(PROTECT);
            add(ARCHIVE);
            add(RATING);
            add(GPS);
            add(XMP);
        }
    };
    private static final String STORAGE_CONTENTS_DATA_SET = "ContentsDataSet";
    private static final String CONTENTS_VERSION = "ContentsVersion";
    private ContentsDataSet mContentsDataSet;

    public static ContentsEditDialogFragment newInstance(Fragment target, ContentsDataSet contentsDataSet, String version){
        ContentsEditDialogFragment instance = new ContentsEditDialogFragment();
        instance.setTargetFragment(target, 0);
        Bundle arguments = new Bundle();
        arguments.putSerializable(STORAGE_CONTENTS_DATA_SET, contentsDataSet);
        arguments.putString(CONTENTS_VERSION, version);
        instance.setArguments(arguments);
        return instance;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle arguments) {
        AlertDialog.Builder dialogBuilder = new AlertDialog.Builder(getActivity());

        if (getArguments() != null) {
            mContentsDataSet = (ContentsDataSet) getArguments().getSerializable(STORAGE_CONTENTS_DATA_SET);
            if(mContentsDataSet != null) {
                dialogBuilder.setTitle(mContentsDataSet.getName());

                ListView listView = new ListView(getActivity());
                //Delete elements because different versions have different EDIT_MENU.
                if(Constants.CCAPI.VER100.equals(getArguments().getString(CONTENTS_VERSION)) || Constants.CCAPI.VER110.equals(getArguments().getString(CONTENTS_VERSION))){
                    EDIT_MENU.remove(EDIT_MENU.indexOf(GPS));
                }
                if(Constants.CCAPI.VER100.equals(getArguments().getString(CONTENTS_VERSION)) || Constants.CCAPI.VER110.equals(getArguments().getString(CONTENTS_VERSION))|| Constants.CCAPI.VER120.equals(getArguments().getString(CONTENTS_VERSION))){
                    EDIT_MENU.remove(EDIT_MENU.indexOf(XMP));
                }
                ArrayAdapter arrayAdapter = new ArrayAdapter<>(getActivity(), android.R.layout.simple_list_item_1, EDIT_MENU);
                listView.setAdapter(arrayAdapter);
                listView.setOnItemClickListener(this);
                dialogBuilder.setView(listView);
            }
        }
        return dialogBuilder.create();
    }

    @Override
    public void onPause() {
        super.onPause();
        dismiss();
    }

    @Override
    public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
        ListView listView = (ListView) parent;
        String menu = (String) listView.getItemAtPosition(position);

        switch(menu){
            case ROTATE: {
                ContentsSpinnerDialogFragment dialog = ContentsSpinnerDialogFragment.newInstance(
                        getTargetFragment(),
                        ContentsSpinnerDialogFragment.ActionType.ACTION_ROTATE,
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            case PROTECT: {
                ContentsSpinnerDialogFragment dialog = ContentsSpinnerDialogFragment.newInstance(
                        getTargetFragment(),
                        ContentsSpinnerDialogFragment.ActionType.ACTION_PROTECT,
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            case ARCHIVE: {
                ContentsSpinnerDialogFragment dialog = ContentsSpinnerDialogFragment.newInstance(
                        getTargetFragment(),
                        ContentsSpinnerDialogFragment.ActionType.ACTION_ARCHIVE,
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            case RATING: {
                ContentsSpinnerDialogFragment dialog = ContentsSpinnerDialogFragment.newInstance(
                        getTargetFragment(),
                        ContentsSpinnerDialogFragment.ActionType.ACTION_RATING,
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            case GPS: {
                ContentsGPSSettingDialogFragment dialog = ContentsGPSSettingDialogFragment.newInstance(
                        getTargetFragment(),
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            case XMP: {
                ContentsXMPSettingDialogFragment dialog = ContentsXMPSettingDialogFragment.newInstance(
                        getTargetFragment(),
                        mContentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), mContentsDataSet.getName());
                break;
            }
            default:
                break;
        }

        dismiss();
    }
}
