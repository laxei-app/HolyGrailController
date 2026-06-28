package com.canon.ccapisample;


import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.support.v4.app.Fragment;
import android.support.v4.app.FragmentManager;
import android.support.v4.app.FragmentTransaction;
import android.support.v4.provider.DocumentFile;
import android.util.Log;
import android.view.ContextMenu;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.GridView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import static android.app.Activity.RESULT_OK;
import static com.canon.ccapisample.Constants.CCAPI.DIR_PATH_DEPTH;
import static com.canon.ccapisample.Constants.CCAPI.Field.ACTION;
import static com.canon.ccapisample.Constants.CCAPI.Field.STATUS;
import static com.canon.ccapisample.Constants.CCAPI.Field.ADDED_CONTENTS;
import static com.canon.ccapisample.Constants.CCAPI.Field.CARD_FORMAT;
import static com.canon.ccapisample.Constants.CCAPI.Field.CONTENTS_NUMBER;
import static com.canon.ccapisample.Constants.CCAPI.Field.CONTENTS_PATH;
import static com.canon.ccapisample.Constants.CCAPI.Field.CONTENTS_URL;
import static com.canon.ccapisample.Constants.CCAPI.Field.DELETED_CONTENTS;
import static com.canon.ccapisample.Constants.CCAPI.Field.MESSAGE;
import static com.canon.ccapisample.Constants.CCAPI.Field.PAGE_NUMBER;
import static com.canon.ccapisample.Constants.CCAPI.Field.RECBUTTON;
import static com.canon.ccapisample.Constants.CCAPI.Field.STORAGE_LIST;
import static com.canon.ccapisample.Constants.CCAPI.Field.STORAGE_NAME;
import static com.canon.ccapisample.Constants.CCAPI.Field.STORAGE_PATH;
import static com.canon.ccapisample.Constants.CCAPI.Field.STORAGE_URL;
import static com.canon.ccapisample.Constants.CCAPI.Field.SUPPLEMENT;
import static com.canon.ccapisample.Constants.CCAPI.Key.CONTENTS;
import static com.canon.ccapisample.Constants.CCAPI.Method.DELETE;
import static com.canon.ccapisample.Constants.CCAPI.Method.GET;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_ARRAY;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_EMBEDDED;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_DISPLAY;
import static com.canon.ccapisample.Constants.ContentsViewer.SAVE_ORIGINAL;


/**
 * A simple {@link Fragment} subclass.
 * Use the {@link ContentsViewerFragment#newInstance} factory method to
 * create an instance of this fragment.
 */
public class ContentsViewerFragment extends Fragment implements WebAPIResultListener, EventListener, ChunkResultListener, View.OnClickListener, AdapterView.OnItemSelectedListener, GridView.OnItemClickListener {
    private static final String TAG = ContentsViewerFragment.class.getSimpleName();

    private enum ContextMenuID {
        URL,
        SAVE,
        ORIGINAL,
        DISPLAY,
        EMBEDDED,
        INFO,
        EDIT,
        DELETE,
    }

    private Button mDirectoryDeleteButton;
    private boolean mIsDirectoryDeleteWait = false;
    private Spinner mSaveTypeSpinner;
    private Spinner mStorageSpinner;
    private Spinner mDirectorySpinner;
    private Spinner mTypeSpinner;
    private Spinner mOrderSpinner;
    private Spinner mPageSpinner;
    private Spinner mKindSpinner;
    private TextView mContentsNumText;
    private Button mSaveButton;
    private TextView mSaveDirectoryText;

    private WebAPI mWebAPI;
    private Handler mHandler;
    private final List<ContentsDataSet> mStorageDataSetList = new ArrayList<>();
    private final List<ContentsDataSet> mDirectoryDataSetList = new ArrayList<>();
    private final List<ContentsDataSet> mContentsDataSetList = new ArrayList<>();
    private ContentsAdapter mContentsAdapter;
    private ArrayAdapter<String> mStorageArrayAdapter;
    private ArrayAdapter<String> mDirectoryArrayAdapter;
    private ArrayAdapter<Integer> mPageArrayAdapter;
    private boolean mIsFromUserSelection = false;
    private ProgressDialogFragment mProgressDialog = null;
    private ApplicationInfo mAppInfo;

    private static final int OPEN_DOCUMENT_REQUEST_CODE = 100;

    private class ContentsDownloaderThread extends Thread {
        private final List<ContentsDataSet> mContentsDataSetList;
        private final String mQuery;
        private final String mType;
        private final Handler mHandler;
        private final Uri mSaveDirectoryUri;

        ContentsDownloaderThread(List<ContentsDataSet> contentsDataSetList, String query, String type, Uri saveDirectoryUri) {
            mContentsDataSetList = contentsDataSetList;
            mQuery = query;
            mType = type;
            mHandler = new Handler();
            mSaveDirectoryUri = saveDirectoryUri;
        }

        private void notifyThread(){
            synchronized (this) {
                this.notifyAll();
            }
        }

        @Override
        public void run() {
            int num = 0;

            DocumentFile mSaveToDir = DocumentFile.fromTreeUri(getActivity(), mSaveDirectoryUri);
            if (mSaveToDir == null || mSaveToDir.canWrite() == false) {
                Log.d(TAG, "Not exist SaveDirectory.");
                return;
            }

            ContentsDownloader contentsDownloader = new ContentsDownloader(mHandler);

            Log.d(TAG, "ContentsDownloaderThread begin.");

            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    Log.d(TAG, "ContentsDownloaderThread window not touchable.");
                    if(getActivity() != null) {
                        getActivity().getWindow().setFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
                    }
                }
            });

            for(final ContentsDataSet contentsDataSet : mContentsDataSetList){
                num++;

                // Display a dialog -> Get or save an image  -> Remove the dialog
                String name;
                switch (mType){
                    case SAVE_DISPLAY:
                    case SAVE_EMBEDDED:
                        name = contentsDataSet.getNameNoExtension() + ".JPG";
                        break;
                    case SAVE_ORIGINAL:
                    default:
                        name = contentsDataSet.getName();
                        break;
                }

                String url = contentsDataSet.getUrl() + mQuery;
                String dialogTitle = name + " : (" + num + "/" + mContentsDataSetList.size() + ")";

                // Do nothing, if the activity is discarded.
                if(getActivity() != null) {
                    contentsDownloader.execute(getActivity(), name, url, mSaveDirectoryUri, dialogTitle, new WebAPIResultListener() {
                        @Override
                        public void onWebAPIResult(WebAPIResultDataSet result) {
                            if (result.isError()) {
                                interrupt();
                            } else {
                                notifyThread();
                            }
                        }
                    });

                    try {
                        // Wait until the above series of processing is completed.
                        Log.d(TAG, "ContentsDownloaderThread wait.");
                        synchronized (this) {
                            this.wait();
                        }
                        Log.d(TAG, "ContentsDownloaderThread resume.");
                    }
                    catch (InterruptedException e) {
                        // Terminate the processing, if an error occurs in the middle.
                        Log.d(TAG, "ContentsDownloaderThread InterruptedException.");
                        e.printStackTrace();
                        break;
                    }
                }
            }

            Log.d(TAG, "ContentsDownloaderThread end.");

            // Cancel prohibition of screen operation.
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    Log.d(TAG, "ContentsDownloaderThread window touchable.");
                    if(getActivity() != null) {
                        getActivity().getWindow().clearFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE);
                    }
                }
            });
        }
    }

    public ContentsViewerFragment() {
        // Required empty public constructor
    }

    /**
     * Use this factory method to create a new instance of
     * this fragment using the provided parameters.
     *
     * @return A new instance of fragment ContentsViewerFragment.
     */
    public static ContentsViewerFragment newInstance() {
        return new ContentsViewerFragment();
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.d(TAG, "onCreate");
        mWebAPI = WebAPI.getInstance();
        mHandler = new Handler();
        mAppInfo = (ApplicationInfo)getActivity().getApplication();
        ImageProcessor imageProcessor = new ImageProcessor();
        mContentsAdapter = new ContentsAdapter(getContext(), mContentsDataSetList, imageProcessor);
        mStorageArrayAdapter = new ArrayAdapter<>(getActivity(), android.R.layout.simple_spinner_item);
        mDirectoryArrayAdapter = new ArrayAdapter<>(getActivity(), android.R.layout.simple_spinner_item);
        mPageArrayAdapter = new ArrayAdapter<>(getActivity(), android.R.layout.simple_spinner_item);
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container,
                             Bundle savedInstanceState) {
        Log.d(TAG, "onCreateView");

        View view = inflater.inflate(R.layout.fragment_contents_viewer, container, false);

        GridView gridView = view.findViewById(R.id.ContentsGridView);
        gridView.setAdapter(mContentsAdapter);
        gridView.setOnScrollListener(mContentsAdapter);
        gridView.setNumColumns(3);
        gridView.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        gridView.setOnItemClickListener(this);

        // Register a context menu which opens by long press.
        registerForContextMenu(gridView);

        mSaveTypeSpinner = view.findViewById(R.id.SaveTypeSpinner);
        mStorageSpinner = view.findViewById(R.id.StorageSpinner);
        mDirectorySpinner = view.findViewById(R.id.DirectorySpinner);
        mTypeSpinner = view.findViewById(R.id.TypeSpinner);
        mOrderSpinner = view.findViewById(R.id.OrderSpinner);
        mPageSpinner = view.findViewById(R.id.PageSpinner);
        mKindSpinner = view.findViewById(R.id.KindSpinner);
        mContentsNumText = view.findViewById(R.id.ContentsNumText);

        mOrderSpinner.setEnabled(false);

        mSaveTypeSpinner.setAdapter(new ArrayAdapter<>(getActivity(), android.R.layout.simple_spinner_item, SAVE_ARRAY));
        mStorageSpinner.setAdapter(mStorageArrayAdapter);
        mDirectorySpinner.setAdapter(mDirectoryArrayAdapter);
        mPageSpinner.setAdapter(mPageArrayAdapter);
        mSaveDirectoryText = view.findViewById(R.id.SaveDirectory);

        // The OnItemSelectedListener operates even without the user's operation.
        // So, prepare a determination flag.
        // And, it is activated when the user touch a spinner.
        View.OnTouchListener onTouchListener = new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                Log.d(TAG, "Spinner onTouchListener");
                mIsFromUserSelection = true;
                return false;
            }
        };

        // OnTouchListener
        mStorageSpinner.setOnTouchListener(onTouchListener);
        mDirectorySpinner.setOnTouchListener(onTouchListener);
        mTypeSpinner.setOnTouchListener(onTouchListener);
        mKindSpinner.setOnTouchListener(onTouchListener);

        // OnItemSelectedListener
        mStorageSpinner.setOnItemSelectedListener(this);
        mDirectorySpinner.setOnItemSelectedListener(this);
        mTypeSpinner.setOnItemSelectedListener(this);
        mKindSpinner.setOnItemSelectedListener(this);

        // OnClickListener
        view.findViewById(R.id.GetContentsButton).setOnClickListener(this);
        mDirectoryDeleteButton = view.findViewById(R.id.DirectoryDeleteButton);
        mDirectoryDeleteButton.setOnClickListener(this);
        mSaveButton = view.findViewById(R.id.SaveButton);
        mSaveButton.setOnClickListener(this);
        view.findViewById(R.id.SelectButton).setOnClickListener(this);

        Uri uri = mAppInfo.getSavePathUri();
        if (uri != null) {
            DocumentFile file = DocumentFile.fromTreeUri(getContext(), uri);
            mSaveDirectoryText.setText(file.getName());
        }
        if (mSaveDirectoryText.getText().toString().equals("")) {
            mSaveButton.setEnabled(false);
        }

        if(mContentsDataSetList.size() == 0) {
            // Get storage information.
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.GET_DEVICESTATUS_STORAGE, null, this));
        }

        return view;
    }

    @Override
    public void onResume() {
        super.onResume();
        Log.d(TAG, "onResume");
    }

    @Override
    public void onPause() {
        super.onPause();
        Log.d(TAG, "onPause");
    }

    @Override
    public void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        Log.d(TAG, "onSaveInstanceState");
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy");
    }

    @Override
    public void onClick(View v) {
        if (v != null) {
            switch (v.getId()) {
                case R.id.SaveButton: {
                    if(mSaveTypeSpinner.getSelectedItem() != null) {
                        String type = (String) mSaveTypeSpinner.getSelectedItem();
                        String query = "";

                        switch (type){
                            case SAVE_DISPLAY:
                                query = "?kind=display";
                                break;
                            case SAVE_EMBEDDED:
                                query = "?kind=embedded";
                                break;
                            case SAVE_ORIGINAL:
                            default:
                                break;
                        }

                        ContentsDownloaderThread thread = new ContentsDownloaderThread(mContentsDataSetList, query, type, mAppInfo.getSavePathUri());
                        thread.start();
                    }
                    break;
                }
                case R.id.GetContentsButton: {
                    getContents();
                    break;
                }
                case R.id.DirectoryDeleteButton: {
                    if(mDirectorySpinner.getSelectedItem() != null){
                        String directory = (String) mDirectorySpinner.getSelectedItem();
                        if(!directory.equals("all")) {
                            for (ContentsDataSet folderDataSet : mDirectoryDataSetList) {
                                if (folderDataSet.getName().equals(directory)) {

                                    mDirectoryDeleteButton.setEnabled(false);

                                    Bundle args = new Bundle();
                                    String[] params = new String[]{DELETE, folderDataSet.getUrl(), null};
                                    args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
                                    mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
                                    break;
                                }
                            }
                        }
                        else{
                            Toast.makeText(v.getContext(),  "All Directory cannot delete.", Toast.LENGTH_SHORT).show();
                        }
                    }
                    else{
                        Toast.makeText(v.getContext(), "Cannot Execute.", Toast.LENGTH_SHORT).show();
                    }
                    break;
                }
                case R.id.SelectButton: {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    this.startActivityForResult(intent, OPEN_DOCUMENT_REQUEST_CODE);
                }
                default:
                    break;
            }
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
                case R.id.StorageSpinner: {
                    String storageUrl = null;
                    for (ContentsDataSet storageDataSet : mStorageDataSetList) {
                        if (storageDataSet.getName().equals(item)) {
                            storageUrl = storageDataSet.getUrl();
                            break;
                        }
                    }

                    if (storageUrl != null) {
                        // Change the spinner of storage.
                        // Get directories again .
                        Bundle args = new Bundle();
                        String[] params = new String[]{GET, storageUrl, null};
                        args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

                        // Get directories.
                        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
                    }
                    break;
                }
                case R.id.DirectorySpinner: {
                    String dirUrl = null;
                    for (ContentsDataSet directoryDataSet : mDirectoryDataSetList) {
                        if (directoryDataSet.getName().equals(item)) {
                            dirUrl = directoryDataSet.getUrl();
                            break;
                        }
                    }

                    if(dirUrl != null){
                        // Change the spinner of storage.
                        // Get the number of pages again.
                        getPageNumber(dirUrl);
                    }
                    break;
                }
                case R.id.TypeSpinner: {
                    Object directory = mDirectorySpinner.getSelectedItem();
                    if(directory != null){
                        String dirUrl = null;
                        for (ContentsDataSet directoryDataSet : mDirectoryDataSetList) {
                            if (directoryDataSet.getName().equals(directory.toString())) {
                                dirUrl = directoryDataSet.getUrl();
                                break;
                            }
                        }

                        // Get the number of pages again, because "type" has been changed.
                        getPageNumber(dirUrl);
                    }
                    break;
                }
                case R.id.KindSpinner: {
                    if(item.equals("chunked")){
                        mPageSpinner.setEnabled(false);
                        mOrderSpinner.setEnabled(true);
                    }
                    else{
                        mOrderSpinner.setEnabled(false);
                        mPageSpinner.setEnabled(true);
                    }
                    break;
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

    @Override
    public void onItemClick(AdapterView<?> parent, final View view, final int position, long id) {
        ContentsDataSet contentsDataSet = mContentsDataSetList.get(position);

        if(contentsDataSet.getKind() == ContentsDataSet.Kind.IMAGE) {
            // Only images can be displayed, but movies, etc. can not be displayed.
            FragmentManager manager = getActivity().getSupportFragmentManager();
            manager.beginTransaction()
                    .replace(R.id.container, ContentsShowFragment.newInstance(contentsDataSet, null))
                    .addToBackStack(TAG)
                    .commit();
        }
    }

    @Override
    public void onCreateContextMenu(ContextMenu menu, View v, ContextMenu.ContextMenuInfo menuInfo) {
        super.onCreateContextMenu(menu, v, menuInfo);
        Log.d(TAG, "onCreateContextMenu");

        AdapterView.AdapterContextMenuInfo info = (AdapterView.AdapterContextMenuInfo) menuInfo;
        Log.d(TAG, String.valueOf(info.position));
        Log.d(TAG, mContentsDataSetList.get(info.position).getName());

        menu.setHeaderTitle(mContentsDataSetList.get(info.position).getName());

        menu.add(0, ContextMenuID.URL.ordinal(), 0, "URL");
        if (!mSaveDirectoryText.getText().toString().equals("")) {
            menu.add(0, ContextMenuID.SAVE.ordinal(), 0, "Save");
        }

        if(mContentsDataSetList.get(info.position).getKind() == ContentsDataSet.Kind.IMAGE) {
            // Only images can be displayed, but movies, etc. can not be displayed.
            menu.add(0, ContextMenuID.ORIGINAL.ordinal(), 0, "Original");
        }

        menu.add(0, ContextMenuID.DISPLAY.ordinal(), 0, "Display");
        menu.add(0, ContextMenuID.EMBEDDED.ordinal(), 0, "Embedded");
        menu.add(0, ContextMenuID.INFO.ordinal(), 0, "Info");
        menu.add(0, ContextMenuID.EDIT.ordinal(), 0, "Edit");
        menu.add(0, ContextMenuID.DELETE.ordinal(), 0, "Delete");
    }

    @Override
    public boolean onContextItemSelected(MenuItem item) {
        Log.d(TAG, "onContextItemSelected");
        AdapterView.AdapterContextMenuInfo info = (AdapterView.AdapterContextMenuInfo) item.getMenuInfo();
        ContentsDataSet contentsDataSet = mContentsDataSetList.get(info.position);

        switch (ContextMenuID.values()[item.getItemId()]) {
            case URL: {
                MessageDialogFragment dialog = MessageDialogFragment.newInstance(contentsDataSet.getName(), contentsDataSet.getUrl());
                dialog.show(getActivity().getSupportFragmentManager(), contentsDataSet.getName());
                break;
            }
            case SAVE: {
                ContentsSpinnerDialogFragment dialog = ContentsSpinnerDialogFragment.newInstance(
                        this,
                        ContentsSpinnerDialogFragment.ActionType.ACTION_SAVE,
                        contentsDataSet);
                dialog.setSaveDir(mAppInfo.getSavePathUri());
                dialog.show(getActivity().getSupportFragmentManager(), contentsDataSet.getName());
                break;
            }
            case ORIGINAL: {
                FragmentManager manager = getActivity().getSupportFragmentManager();
                manager.beginTransaction()
                        .replace(R.id.container, ContentsShowFragment.newInstance(contentsDataSet, null))
                        .addToBackStack(TAG)
                        .commit();
                break;
            }
            case DISPLAY: {
                String query = "kind=display";
                FragmentManager manager = getActivity().getSupportFragmentManager();
                manager.beginTransaction()
                        .replace(R.id.container, ContentsShowFragment.newInstance(contentsDataSet, query))
                        .addToBackStack(TAG)
                        .commit();
                break;
            }
            case EMBEDDED: {
                String query = "kind=embedded";
                FragmentManager manager = getActivity().getSupportFragmentManager();
                manager.beginTransaction()
                        .replace(R.id.container, ContentsShowFragment.newInstance(contentsDataSet, query))
                        .addToBackStack(TAG)
                        .commit();
                break;
            }
            case INFO: {
                ContentsInfoDialogFragment dialog = ContentsInfoDialogFragment.newInstance(this, ContentsInfoDialogFragment.ShowType.INFO, contentsDataSet);
                dialog.show(getActivity().getSupportFragmentManager(), contentsDataSet.getName());
                break;
            }
            case EDIT: {
                APIDataSet api = mWebAPI.getAPIData(CONTENTS);
                String version = api.getVersion();
                //The version is passed as an argument because the processing depends on the version.
                //If processing other than EDIT also changes by version, version acquisition is performed outside the case statement.
                ContentsEditDialogFragment dialog = ContentsEditDialogFragment.newInstance(this, contentsDataSet, version);
                dialog.show(getActivity().getSupportFragmentManager(), contentsDataSet.getName());
                break;
            }
            case DELETE: {
                Bundle args = new Bundle();
                String[] params = new String[]{DELETE, contentsDataSet.getUrl(), null};
                args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);
                mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
                break;
            }
            default:
                break;
        }
        return false;
    }

    /**
     * Callback from execute WebAPI
     * @param result HTTP Request result
     */
    @Override
    public void onWebAPIResult(WebAPIResultDataSet result) {
        Log.d(TAG, String.format("%s onWebAPIResult", result.getRequestCode()));
        Context context = getActivity();

        // Do nothing, if the life cycle of the fragment is finished.
        if(context != null) {

            //  Close the ProgressDialog, if it is displayed before requesting.
            if(mProgressDialog != null) {

                mProgressDialog.dismissAllowingStateLoss();
                mProgressDialog = null;

                // Update the GridView.
                mContentsAdapter.notifyDataSetChanged();
            }

            if(result.getRequestCode() == Constants.RequestCode.ACT_WEB_API) {
                // The callback from deletion of directory.
                if(result.getMethod().equals(DELETE)) {
                    if(result.isCancel()){
                        mIsDirectoryDeleteWait = true;
                        Toast.makeText(context, "Delete: " + result.getRequestName() + " Timeout.", Toast.LENGTH_SHORT).show();
                    }else {
                        mDirectoryDeleteButton.setEnabled(true);
                    }
                }
            }

            // The processing of execution result.
            if (result.isError()) {
                if(!result.isCancel()){
                    Toast.makeText(context, result.getErrorMsg(), Toast.LENGTH_SHORT).show();
                }
            }
            else {
                switch (result.getRequestCode()) {
                    case GET_DEVICESTATUS_STORAGE: {

                        // The callback from information acquisition of the storage on the onCreateView().
                        mStorageDataSetList.clear();
                        mStorageArrayAdapter.clear();
                        try {
                            JSONObject jsonObject = new JSONObject(result.getResponseBody());
                            JSONArray storageArray = jsonObject.getJSONArray(STORAGE_LIST);
                            for (int i = 0; i < storageArray.length(); i++) {
                                String name = storageArray.getJSONObject(i).getString(STORAGE_NAME);

                                String url;
                                if(storageArray.getJSONObject(i).has(STORAGE_PATH)) {
                                    url = storageArray.getJSONObject(i).getString(STORAGE_PATH);
                                }else{
                                    url = storageArray.getJSONObject(i).getString(STORAGE_URL);
                                }
                                ContentsDataSet contentsDataSet = new ContentsDataSet(url);
                                mStorageArrayAdapter.add(name);
                                mStorageDataSetList.add(contentsDataSet);
                            }
                            mStorageArrayAdapter.notifyDataSetChanged();

                            if(mStorageDataSetList.size() != 0) {
                                Bundle args = new Bundle();
                                String[] params = new String[]{GET, mStorageDataSetList.get(0).getUrl(), null};
                                args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

                                // Get directories.
                                mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
                            }
                        }
                        catch (JSONException e) {
                            e.printStackTrace();
                        }
                        break;
                    }
                    case ACT_WEB_API: {

                        // The callback from acquisition of contents or directories.
                        if(result.getMethod().equals(GET)) {
                            if(!result.getResponseBody().isEmpty()) {
                                try {
                                    JSONObject jsonObject = new JSONObject(result.getResponseBody());

                                    if(!jsonObject.isNull(CONTENTS_PATH) || !jsonObject.isNull(CONTENTS_URL) ) {

                                        // Get the list of contents or directories.
                                        ArrayList<ContentsDataSet> directoryDataSetList = new ArrayList<>();
                                        ArrayList<ContentsDataSet> contentsDataSetList = new ArrayList<>();
                                        JSONArray urlArray;
                                        if(jsonObject.has(CONTENTS_PATH)){
                                            urlArray = jsonObject.getJSONArray(CONTENTS_PATH);
                                        }else{
                                            urlArray = jsonObject.getJSONArray(CONTENTS_URL);
                                        }

                                        for (int i = 0; i < urlArray.length(); i++) {
                                            ContentsDataSet contentsDataSet = new ContentsDataSet(urlArray.getString(i));
                                            if (contentsDataSet.getKind() == ContentsDataSet.Kind.DIR) {
                                                directoryDataSetList.add(contentsDataSet);
                                            }
                                            else{
                                                contentsDataSetList.add(contentsDataSet);
                                            }
                                        }

                                        if (directoryDataSetList.size() != 0) {

                                            // The callback from the list acquisition of directories.
                                            mDirectoryDataSetList.clear();
                                            mDirectoryDataSetList.addAll(directoryDataSetList);
                                            ArrayList<String> directoryList = new ArrayList<>();
                                            for (ContentsDataSet directoryDataSet : mDirectoryDataSetList) {
                                                directoryList.add(directoryDataSet.getName());
                                            }

                                            mDirectoryArrayAdapter.clear();
                                            mDirectoryArrayAdapter.addAll(directoryList);
                                            mDirectoryArrayAdapter.notifyDataSetChanged();
                                            mDirectorySpinner.setSelection(0);

                                            getPageNumber(mDirectoryDataSetList.get(0).getUrl());
                                        }
                                        else{

                                            // The callback from the list acquisition of contents.
                                            mContentsDataSetList.addAll(contentsDataSetList);

                                            // Update the GridView.
                                            mContentsAdapter.notifyDataSetChanged();
                                        }
                                    }
                                    else if(!jsonObject.isNull(PAGE_NUMBER) && !jsonObject.isNull(CONTENTS_NUMBER)){

                                        // Get the number of the directory's pages/contents.
                                        int page = jsonObject.getInt(PAGE_NUMBER);
                                        ArrayList<Integer> pageList = new ArrayList<>();
                                        for (int i = 0; i < page; i++) {
                                            pageList.add(i+1);
                                        }
                                        mPageArrayAdapter.clear();
                                        mPageArrayAdapter.addAll(pageList);
                                        mPageArrayAdapter.notifyDataSetChanged();
                                        mPageSpinner.setSelection(0);

                                        Integer contents = jsonObject.getInt(CONTENTS_NUMBER);
                                        mContentsNumText.setText(String.format(Locale.US, "%d", contents));
                                    }
                                }
                                catch (JSONException e) {
                                    e.printStackTrace();
                                }
                            }
                        }

                        // The callback from the deletion of contents or directories.
                        else if(result.getMethod().equals(DELETE)){
                            Toast.makeText(context, "Delete Success.", Toast.LENGTH_SHORT).show();
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        else{
            Log.d(TAG, String.format("%s Activity is Null.", result.getRequestCode()));
        }
    }

    @Override
    public void onNotifyEvent(String response){
        try {
            JSONObject jsonObject = new JSONObject(response);
            if(jsonObject.length() != 0) {
                Iterator<String> iterator = jsonObject.keys();
                while(iterator.hasNext()) {
                    String key = iterator.next();
                    Log.d(TAG, String.format("onNotifyEvent : %s : %s", key, jsonObject.getString(key)));

                    switch (key) {
                        case ADDED_CONTENTS:

                            // Do nothing, if this event is notified at the same time the format event.
                            if (jsonObject.isNull(CARD_FORMAT)) {
                                addedContents(jsonObject.getJSONArray(key));
                            }
                            break;
                        case DELETED_CONTENTS:

                            String path = jsonObject.getString(DELETED_CONTENTS);

                            if(mIsDirectoryDeleteWait){
                                String[] part = path.split("/");
                                // Directory deletion processing completed when storage path is notified
                                if(part.length == DIR_PATH_DEPTH){
                                    mIsDirectoryDeleteWait = false;
                                    mDirectoryDeleteButton.setEnabled(true);
                                }
                            }
                            // Do nothing, if this event is notified at the same time the format event.
                            if (jsonObject.isNull(CARD_FORMAT)) {
                                deletedContents(jsonObject.getJSONArray(key));
                            }
                            break;
                        case CARD_FORMAT:
                            deletedContents(jsonObject.getJSONArray(key));
                            break;
                        case RECBUTTON:
                            JSONObject rec = new JSONObject(jsonObject.getString(RECBUTTON));

                            /* Ver. 1.0.0 */
                            if (!rec.isNull(ACTION)) {
                                if (rec.getString(ACTION).equals("start")) {

                                    mWebAPI.cancelAllSerialRequest();

                                    // Return to the MainActivity's display by the DisconnectListener.
                                    Activity activity = getActivity();
                                    if (activity instanceof DisconnectListener) {
                                        ((DisconnectListener) activity).onNotifyDisconnect("Rec started.", false);
                                    }
                                }
                            /* Ver. 1.1.0 or later */
                            }else if(!rec.isNull(STATUS)) {
                                if (rec.getString(STATUS).equals("start")) {

                                    mWebAPI.cancelAllSerialRequest();

                                    // Return to the MainActivity's display by the DisconnectListener.
                                    Activity activity = getActivity();
                                    if (activity instanceof DisconnectListener) {
                                        ((DisconnectListener) activity).onNotifyDisconnect("Rec started.", false);
                                    }
                                }
                            }

                            break;
                        case SUPPLEMENT:
                            JSONObject obj = jsonObject.getJSONObject(SUPPLEMENT);
                            // When "deletedcontents" is notified as supplementary information, the directory deletion processing is completed.
                            if(!obj.isNull(DELETED_CONTENTS)) {
                                String deletedcontents_messeage = obj.getString(DELETED_CONTENTS);
                                if(deletedcontents_messeage.length() > 0){
                                    Toast.makeText(getContext(), DELETED_CONTENTS + ": " + deletedcontents_messeage, Toast.LENGTH_SHORT).show();
                                }
                                if(mIsDirectoryDeleteWait) {
                                    mIsDirectoryDeleteWait = false;
                                    mDirectoryDeleteButton.setEnabled(true);
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
    }

    private void addedContents(JSONArray pathArray) throws JSONException{
        boolean addedDirectory = false;
        String addedStoragePath = null;
        Object selectedStorage = mStorageSpinner.getSelectedItem();

        for(int i = 0; i < pathArray.length(); i++){
            String path = pathArray.getString(i);
            Log.d(TAG, String.format("addedcontents : %s", path));
            ContentsDataSet contentsDataSet = new ContentsDataSet(path);
            String ext = contentsDataSet.getExtension();

            // Determine if added contents are contents of the storage which is choosing in a spinner or not.
            if(selectedStorage != null && path.contains(selectedStorage.toString())){

                // Determine if added contents are a directory or not.
                if (ext.isEmpty()) {
                    addedStoragePath = contentsDataSet.getUrl().replace("/" + contentsDataSet.getName(), "");
                    addedDirectory = true;
                }
            }
        }

        if(addedDirectory){

            // Get directories from the storage again.
            Bundle args = new Bundle();
            String[] params = new String[]{GET, addedStoragePath, null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

            // Get directories.
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
        }
    }

    private void deletedContents(JSONArray pathArray) throws JSONException{
        String deletedDirStorage = null;
        boolean isDeletedDirectory = false;
        Object selectedStorage = mStorageSpinner.getSelectedItem();

        for(int i = 0; i < pathArray.length(); i++){
            String path = pathArray.getString(i);
            Log.d(TAG, String.format("deletedcontents : %s", path));

            // Determine if the notification is about the storage which is choosing in a spinner or not.
            if(selectedStorage != null && path.endsWith(selectedStorage.toString())){
                isDeletedDirectory = true;
                if(path.contains(mWebAPI.getUrl())) {
                    deletedDirStorage = path;
                }else{
                    deletedDirStorage = mWebAPI.getUrl().replace("/ccapi", "") + path;
                }
            }
        }

        if(isDeletedDirectory){

            // Get directories again.
            Bundle args = new Bundle();
            String[] params = new String[]{GET, deletedDirStorage, null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

            // Get directories.
            mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
        }
    }

    private void getPageNumber(String url){
        String query = "?kind=number";

        Object type = mTypeSpinner.getSelectedItem();
        if(type != null){
            query += "&type=" + type.toString();
        }

        Bundle args = new Bundle();
        String[] params = new String[]{GET, url + query, null};
        args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

        // Get the number of directory's pages.
        mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
    }

    @Override
    public boolean onChunkResult(byte[] bytes) {
        boolean ret = true;
        final Context context = getActivity();

        // Confirm the existence of fragment.
        if(context != null) {

            // Send the error message or the URL list every chunk.
            if (bytes != null) {
                try {
                    String body = new String(bytes, StandardCharsets.UTF_8);
                    Log.d(TAG, String.format("onChunkResult : %s", body));

                    JSONObject jsonObject = new JSONObject(body);
                    if (!jsonObject.isNull(CONTENTS_PATH) || !jsonObject.isNull(CONTENTS_URL) ) {
                        JSONArray urlArray;
                        if(!jsonObject.isNull(CONTENTS_PATH)){
                            urlArray = jsonObject.getJSONArray(CONTENTS_PATH);
                        }else{
                            urlArray = jsonObject.getJSONArray(CONTENTS_URL);
                        }
                        for (int i = 0; i < urlArray.length(); i++) {
                            ContentsDataSet contentsDataSet = new ContentsDataSet(urlArray.getString(i));
                            if (contentsDataSet.getKind() != ContentsDataSet.Kind.DIR) {
                                // Reflect on the contents list.
                                // Update the GridView when the dialog is closed on the onWebAPIResult().
                                mContentsDataSetList.add(contentsDataSet);
                            }
                        }
                    }
                    else if (!jsonObject.isNull(MESSAGE)) {
                        final String message = jsonObject.getString(MESSAGE) + "\nPlease reload.";
                        mHandler.post(new Runnable() {
                            public void run() {
                                Toast.makeText(context, message, Toast.LENGTH_SHORT).show();
                            }
                        });
                    }
                    else{
                        Log.d(TAG, "onChunkResult : Unknown data.");
                    }
                }
                catch (OutOfMemoryError | JSONException e) {
                    e.printStackTrace();
                }
            }
        }
        else{
            // Finish the chunk session, when life cycle of the fragment is finished.
            ret = false;
            Log.d(TAG, "onChunkResult : Context is null.");
        }
        return ret;
    }

    private void getContents(){
        mContentsDataSetList.clear();
        mContentsAdapter.notifyDataSetChanged();

        Object storage = mStorageSpinner.getSelectedItem();
        Object directory = mDirectorySpinner.getSelectedItem();
        Object type = mTypeSpinner.getSelectedItem();
        Object order = mOrderSpinner.getSelectedItem();
        Object kind = mKindSpinner.getSelectedItem();

        if(storage != null && directory != null && type != null && order != null && kind != null) {
            String url = "";
            String query = null;
            Map<String, String> queryMap = new HashMap<>();
            StringBuilder stringBuilder = new StringBuilder();
            boolean isChunked = false;

            if(kind.equals("chunked")){
                isChunked = true;
            }

            for (ContentsDataSet directoryDataSet : mDirectoryDataSetList) {
                if (directoryDataSet.getName().equals(directory)) {
                    url = directoryDataSet.getUrl();
                    break;
                }
            }

            queryMap.put("type", type.toString());
            queryMap.put("kind", kind.toString());

            if(isChunked){
                queryMap.put("order", order.toString());
            }
            else{

                // Check whether the page value is NULL,
                // Because the list is empty when the number of pages returns with 0.
                Object page = mPageSpinner.getSelectedItem();
                if(page != null) {
                    queryMap.put("page", page.toString());
                }
            }

            if (queryMap.size() != 0) {
                for (Map.Entry<String, String> entry : queryMap.entrySet()) {
                    stringBuilder.append(entry.getKey()).append("=").append(entry.getValue()).append("&");
                }
                query = stringBuilder.substring(0, stringBuilder.length() - 1);
            }

            Bundle args = new Bundle();
            String[] params = new String[]{GET, url + "?" + query, null};
            args.putStringArray(Constants.RequestCode.ACT_WEB_API.name(), params);

            mContentsDataSetList.clear();

            if(isChunked) {

                // Display a dialog.
                // Close the dialog on the onWebAPIResult().
                mProgressDialog = ProgressDialogFragment.newInstance(ProgressDialogFragment.Type.Circular, "ContentsViewer", url + "?" + query);
                FragmentTransaction fragmentTransaction = getActivity().getSupportFragmentManager().beginTransaction();
                fragmentTransaction.add(mProgressDialog, null);
                fragmentTransaction.commitAllowingStateLoss();

                // Get a contents list.
                // Receive a contents URL on the onChunkResult().
                mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, false, this, this));
            }
            else{

                // Display a dialog.
                // Close the dialog on the onWebAPIResult().
                mProgressDialog = ProgressDialogFragment.newInstance(ProgressDialogFragment.Type.Circular, "ContentsViewer");
                FragmentTransaction fragmentTransaction = getActivity().getSupportFragmentManager().beginTransaction();
                fragmentTransaction.add(mProgressDialog, null);
                fragmentTransaction.commitAllowingStateLoss();

                // Get a contents list.
                // Receive a contents URL on the onChunkResult().
                mWebAPI.enqueueRequest(new WebAPIQueueDataSet(Constants.RequestCode.ACT_WEB_API, args, this));
            }
        }
        else{
            Log.d(TAG, "getContents : not execute.");
        }
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == OPEN_DOCUMENT_REQUEST_CODE) {
            if (resultCode == RESULT_OK) {
                Uri uri = null;
                if (data != null) {
                    uri = data.getData();
                }

                if (uri != null) {
                    mAppInfo.setSavePathUri(uri);
                    DocumentFile file = DocumentFile.fromTreeUri(getContext(), uri);
                    mSaveDirectoryText.setText(file.getName());
                    mSaveButton.setEnabled(true);
                }
                else {
                    mSaveButton.setEnabled(false);
                }
            }
        }
    }
}
