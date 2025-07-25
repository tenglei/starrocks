// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/load/loadv2/LoadJob.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.load.loadv2;

import com.google.common.base.Joiner;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.google.common.collect.Sets;
import com.google.gson.Gson;
import com.google.gson.annotations.SerializedName;
import com.starrocks.catalog.Database;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.Config;
import com.starrocks.common.DdlException;
import com.starrocks.common.DuplicatedRequestException;
import com.starrocks.common.FeConstants;
import com.starrocks.common.LabelAlreadyUsedException;
import com.starrocks.common.LoadException;
import com.starrocks.common.MetaNotFoundException;
import com.starrocks.common.StarRocksException;
import com.starrocks.common.io.Text;
import com.starrocks.common.io.Writable;
import com.starrocks.common.util.LoadPriority;
import com.starrocks.common.util.LogBuilder;
import com.starrocks.common.util.LogKey;
import com.starrocks.common.util.ProfileManager;
import com.starrocks.common.util.PropertyAnalyzer;
import com.starrocks.common.util.TimeUtils;
import com.starrocks.load.EtlJobType;
import com.starrocks.load.EtlStatus;
import com.starrocks.load.FailMsg;
import com.starrocks.load.FailMsg.CancelType;
import com.starrocks.load.Load;
import com.starrocks.load.LoadConstants;
import com.starrocks.metric.MetricRepo;
import com.starrocks.persist.AlterLoadJobOperationLog;
import com.starrocks.persist.gson.GsonUtils;
import com.starrocks.qe.ConnectContext;
import com.starrocks.qe.QeProcessorImpl;
import com.starrocks.qe.scheduler.Coordinator;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.server.RunMode;
import com.starrocks.server.WarehouseManager;
import com.starrocks.sql.ast.AlterLoadStmt;
import com.starrocks.sql.ast.LoadStmt;
import com.starrocks.task.LeaderTaskExecutor;
import com.starrocks.task.PriorityLeaderTask;
import com.starrocks.task.PriorityLeaderTaskExecutor;
import com.starrocks.thrift.TEtlState;
import com.starrocks.thrift.TLoadInfo;
import com.starrocks.thrift.TReportExecStatusParams;
import com.starrocks.thrift.TUniqueId;
import com.starrocks.transaction.AbstractTxnStateChangeCallback;
import com.starrocks.transaction.BeginTransactionException;
import com.starrocks.transaction.RunningTxnExceedException;
import com.starrocks.transaction.TabletCommitInfo;
import com.starrocks.transaction.TabletFailInfo;
import com.starrocks.transaction.TransactionException;
import com.starrocks.transaction.TransactionState;
import com.starrocks.warehouse.LoadJobWithWarehouse;
import com.starrocks.warehouse.Warehouse;
import com.starrocks.warehouse.cngroup.ComputeResource;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import java.util.stream.Collectors;

public abstract class LoadJob extends AbstractTxnStateChangeCallback implements LoadTaskCallback, Writable, LoadJobWithWarehouse {

    private static final Logger LOG = LogManager.getLogger(LoadJob.class);

    public static final String DPP_NORMAL_ALL = "dpp.norm.ALL";
    public static final String DPP_ABNORMAL_ALL = "dpp.abnorm.ALL";
    public static final String UNSELECTED_ROWS = "unselected.rows";
    public static final String LOADED_BYTES = "loaded.bytes";

    private static final int TASK_SUBMIT_RETRY_NUM = 2;

    @SerializedName("id")
    protected long id;
    // input params
    @SerializedName("d")
    protected long dbId;
    @SerializedName("l")
    protected String label;
    @SerializedName("s")
    protected JobState state = JobState.PENDING;
    @SerializedName("j")
    protected EtlJobType jobType;

    // optional properties
    // timeout second need to be reset in constructor of subclass
    @SerializedName("t")
    protected long timeoutSecond = Config.broker_load_default_timeout_second;
    @SerializedName("lm")
    protected long loadMemLimit = 0;      // default no limit for load memory
    @SerializedName("m")
    protected double maxFilterRatio = 0;
    @SerializedName("st")
    protected boolean strictMode = false; // default is false
    @SerializedName("tz")
    protected String timezone = TimeUtils.DEFAULT_TIME_ZONE;
    @SerializedName("p")
    protected boolean partialUpdate = false;
    @SerializedName("pum")
    protected String partialUpdateMode = "row";
    @SerializedName("pr")
    protected int priority = LoadPriority.NORMAL_VALUE;
    @SerializedName("ln")
    protected long logRejectedRecordNum = 0;

    @SerializedName("c")
    protected long createTimestamp = -1;
    @SerializedName("ls")
    protected long loadStartTimestamp = -1;
    @SerializedName("loadCommittedTimestamp")
    protected long loadCommittedTimestamp = -1;
    @SerializedName("f")
    protected long finishTimestamp = -1;

    @SerializedName("tx")
    protected long transactionId;
    @SerializedName("fm")
    protected FailMsg failMsg;
    protected Map<Long, LoadTask> idToTasks = Maps.newConcurrentMap();
    protected List<String> loadIds = Lists.newArrayList();
    protected Set<Long> finishedTaskIds = Sets.newHashSet();
    @SerializedName("lt")
    protected EtlStatus loadingStatus = new EtlStatus();
    // 0: the job status is pending
    // n/100: n is the number of task which has been finished
    // 99: all of tasks have been finished
    // 100: txn status is visible and load has been finished
    @SerializedName("pg")
    protected int progress;

    @SerializedName("warehouseId")
    protected long warehouseId = WarehouseManager.DEFAULT_WAREHOUSE_ID;
    // no needs to persistent
    protected ComputeResource computeResource = WarehouseManager.DEFAULT_RESOURCE;

    @SerializedName("mc")
    protected String mergeCondition;
    @SerializedName("jo")
    protected JSONOptions jsonOptions = new JSONOptions();

    @SerializedName("user")
    protected String user = "";

    public int getProgress() {
        return this.progress;
    }

    // non-persistence
    // This param is set true during txn is committing.
    // During committing, the load job could not be cancelled.
    protected boolean isCommitting = false;

    protected ReentrantReadWriteLock lock = new ReentrantReadWriteLock(true);

    // this request id is only used for checking if a load begin request is a duplicate request.
    protected TUniqueId requestId;

    protected int retryTime = 2;

    // only for persistence param. see readFields() for usage
    private boolean isJobTypeRead = false;

    protected boolean startLoad = false;

    // only for log replay
    public LoadJob() {
    }

    public LoadJob(long dbId, String label) {
        this.id = GlobalStateMgr.getCurrentState().getNextId();
        this.dbId = dbId;
        this.label = label;
        if (ConnectContext.get() != null) {
            this.createTimestamp = ConnectContext.get().getStartTime();
        } else {
            // only for test used
            this.createTimestamp = System.currentTimeMillis();
        }
    }

    protected void readLock() {
        lock.readLock().lock();
    }

    protected void readUnlock() {
        lock.readLock().unlock();
    }

    protected void writeLock() {
        lock.writeLock().lock();
    }

    protected void writeUnlock() {
        lock.writeLock().unlock();
    }

    @Override
    public long getCurrentWarehouseId() {
        return warehouseId;
    }

    public void setWarehouseId(long warehouseId) {
        this.warehouseId = warehouseId;
    }

    @Override
    public boolean isFinal() {
        return isCompleted();
    }

    @Override
    public long getFinishTimestampMs() {
        return getFinishTimestamp();
    }

    public long getId() {
        return id;
    }

    // unit test
    public void setId(long id) {
        this.id = id;
    }

    protected void setLabel(String label) {
        this.label = label;
    }

    public Database getDb() throws MetaNotFoundException {
        // get db
        Database db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb(dbId);
        if (db == null) {
            throw new MetaNotFoundException("Database " + dbId + " already has been deleted");
        }
        return db;
    }

    public long getDbId() {
        return dbId;
    }

    public String getLabel() {
        return label;
    }

    public JobState getState() {
        return state;
    }

    protected void setState(JobState state) {
        this.state = state;
    }

    public JobState getAcutalState() {
        if (state == JobState.LOADING) {
            if (startLoad) {
                return JobState.LOADING;
            } else {
                return JobState.QUEUEING;
            }
        } else {
            return state;
        }
    }

    public EtlJobType getJobType() {
        return jobType;
    }

    protected long getDeadlineMs() {
        return createTimestamp + timeoutSecond * 1000;
    }

    protected void reset() {
        createTimestamp = System.currentTimeMillis();
        idToTasks.clear();
        finishedTaskIds.clear();
        loadingStatus.reset();
    }

    public boolean isTimeout() {
        return System.currentTimeMillis() > getDeadlineMs();
    }

    public long getFinishTimestamp() {
        return finishTimestamp;
    }

    public long getTransactionId() {
        return transactionId;
    }

    public long getLoadStartTimestamp() {
        return loadStartTimestamp;
    }

    public void initLoadProgress(TUniqueId loadId, Set<TUniqueId> fragmentIds, List<Long> relatedBackendIds) {
        loadingStatus.getLoadStatistic().initLoad(loadId, fragmentIds, relatedBackendIds);
        startLoad = true;
        loadStartTimestamp = System.currentTimeMillis();
    }

    public void updateProgress(TReportExecStatusParams params) {
        loadingStatus.getLoadStatistic().updateLoadProgress(params);
    }

    public void setLoadFileInfo(int fileNum, long fileSize) {
        loadingStatus.setLoadFileInfo(fileNum, fileSize);
    }

    public void updateScanRangeNum(long scanRangeNum) {
        loadingStatus.updateScanRangeNum(scanRangeNum);
    }

    public TUniqueId getRequestId() {
        return requestId;
    }

    /**
     * Show table names for frontend
     * If table name could not be found by id, the table id will be used instead.
     *
     * @return
     */
    abstract Set<String> getTableNamesForShow();

    /**
     * Return the real table names by table ids.
     * The method is invoked by 'checkAuth' when authorization info is null in job.
     * It is also invoked by 'gatherAuthInfo' which saves the auth info in the constructor of job.
     * Throw MetaNofFoundException when table name could not be found.
     *
     * @return
     */
    public abstract Set<String> getTableNames(boolean noThrow) throws MetaNotFoundException;

    protected abstract List<TabletCommitInfo> getTabletCommitInfos();

    protected abstract List<TabletFailInfo> getTabletFailInfos();

    // return true if the corresponding transaction is done(COMMITTED, FINISHED, CANCELLED)
    public boolean isTxnDone() {
        return state == JobState.COMMITTED || state == JobState.FINISHED || state == JobState.CANCELLED;
    }

    // return true if job is done(FINISHED/CANCELLED/UNKNOWN)
    public boolean isCompleted() {
        return state == JobState.FINISHED || state == JobState.CANCELLED || state == JobState.UNKNOWN;
    }

    public void setJobProperties(Map<String, String> properties) throws DdlException {
        // resource info
        if (ConnectContext.get() != null) {
            loadMemLimit = ConnectContext.get().getSessionVariable().getLoadMemLimit();
        }

        if (ConnectContext.get() != null) {
            user = ConnectContext.get().getQualifiedUser();
        }

        // job properties
        if (properties != null) {
            if (properties.containsKey(LoadStmt.TIMEOUT_PROPERTY)) {
                try {
                    timeoutSecond = Integer.parseInt(properties.get(LoadStmt.TIMEOUT_PROPERTY));
                } catch (NumberFormatException e) {
                    throw new DdlException("Timeout is not INT", e);
                }
            }

            if (properties.containsKey(LoadStmt.MAX_FILTER_RATIO_PROPERTY)) {
                try {
                    maxFilterRatio = Double.parseDouble(properties.get(LoadStmt.MAX_FILTER_RATIO_PROPERTY));
                } catch (NumberFormatException e) {
                    throw new DdlException("Max filter ratio is not DOUBLE", e);
                }
            }

            if (properties.containsKey(LoadStmt.LOAD_DELETE_FLAG_PROPERTY)) {
                throw new DdlException("delete flag is not supported");
            }
            if (properties.containsKey(LoadStmt.PARTIAL_UPDATE)) {
                partialUpdate = Boolean.valueOf(properties.get(LoadStmt.PARTIAL_UPDATE));
            }
            if (properties.containsKey(LoadStmt.PARTIAL_UPDATE_MODE)) {
                partialUpdateMode = properties.get(LoadStmt.PARTIAL_UPDATE_MODE);
            }
            if (properties.containsKey(LoadStmt.MERGE_CONDITION)) {
                mergeCondition = properties.get(LoadStmt.MERGE_CONDITION);
            }

            if (properties.containsKey(LoadStmt.LOAD_MEM_LIMIT)) {
                try {
                    loadMemLimit = Long.parseLong(properties.get(LoadStmt.LOAD_MEM_LIMIT));
                } catch (NumberFormatException e) {
                    throw new DdlException("Execute memory limit is not Long", e);
                }
            }

            if (properties.containsKey(LoadStmt.STRICT_MODE)) {
                strictMode = Boolean.valueOf(properties.get(LoadStmt.STRICT_MODE));
            }

            if (properties.containsKey(LoadStmt.TIMEZONE)) {
                timezone = properties.get(LoadStmt.TIMEZONE);
            } else if (ConnectContext.get() != null) {
                // get timezone for session variable
                timezone = ConnectContext.get().getSessionVariable().getTimeZone();
            }

            if (properties.containsKey(LoadStmt.PRIORITY)) {
                priority = LoadPriority.priorityByName(properties.get(LoadStmt.PRIORITY));
            }

            if (properties.containsKey(LoadStmt.LOG_REJECTED_RECORD_NUM)) {
                logRejectedRecordNum = Long.parseLong(properties.get(LoadStmt.LOG_REJECTED_RECORD_NUM));
            }

            if (RunMode.isSharedDataMode()) {
                if (properties.containsKey(PropertyAnalyzer.PROPERTIES_WAREHOUSE)) {
                    String warehouseName = properties.get(PropertyAnalyzer.PROPERTIES_WAREHOUSE);
                    Warehouse warehouse =
                            GlobalStateMgr.getCurrentState().getWarehouseMgr().getWarehouse(warehouseName);
                    warehouseId = warehouse.getId();
                } else {
                    warehouseId = ConnectContext.get().getCurrentWarehouseId();
                }
            }

            if (properties.containsKey(LoadStmt.STRIP_OUTER_ARRAY)) {
                jsonOptions.stripOuterArray = Boolean.parseBoolean(properties.get(LoadStmt.STRIP_OUTER_ARRAY));
            }

            if (properties.containsKey(LoadStmt.JSONPATHS)) {
                jsonOptions.jsonPaths = properties.get(LoadStmt.JSONPATHS);
            }

            if (properties.containsKey(LoadStmt.JSONROOT)) {
                jsonOptions.jsonRoot = properties.get(LoadStmt.JSONROOT);
            }
        } else {
            if (RunMode.isSharedDataMode()) {
                // if no properties set, we should still set warehouse here
                warehouseId = ConnectContext.get().getCurrentWarehouseId();
            }
        }
    }

    public void isJobTypeRead(boolean jobTypeRead) {
        isJobTypeRead = jobTypeRead;
    }

    public void beginTxn()
            throws LabelAlreadyUsedException, BeginTransactionException,
            RunningTxnExceedException, AnalysisException, DuplicatedRequestException {
    }

    /**
     * create pending task for load job and add pending task into pool
     * if job has been cancelled, this step will be ignored
     *
     * @throws LabelAlreadyUsedException  the job is duplicated
     * @throws RunningTxnExceedException  the limit of load job is exceeded
     * @throws AnalysisException          there are error params in job
     * @throws DuplicatedRequestException
     */
    public void execute() throws LabelAlreadyUsedException, RunningTxnExceedException, AnalysisException,
            DuplicatedRequestException, LoadException {
        writeLock();
        try {
            unprotectedExecute();
        } finally {
            writeUnlock();
        }
    }

    public void unprotectedExecute() throws LabelAlreadyUsedException, RunningTxnExceedException, AnalysisException,
            DuplicatedRequestException, LoadException {
        // check if job state is pending
        if (state != JobState.PENDING) {
            return;
        }
        try {
            // the limit of job will be restrict when begin txn
            beginTxn();
            unprotectedExecuteJob();
            // update spark load job state from PENDING to ETL when pending task is finished
            if (jobType != EtlJobType.SPARK) {
                unprotectedUpdateState(JobState.LOADING);
            }
        } catch (BeginTransactionException e) {
            throw new LoadException(e.getMessage());
        }
    }

    protected void submitTask(LeaderTaskExecutor executor, LoadTask task) throws LoadException {
        int retryNum = 0;
        while (!executor.submit(task)) {
            LOG.warn("submit load task failed. try to resubmit. job id: {}, task id: {}, retry: {}",
                    id, task.getSignature(), retryNum);
            if (++retryNum > TASK_SUBMIT_RETRY_NUM) {
                throw new LoadException("submit load task failed");
            }

            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                LOG.warn("Failed to execute submitTask", e);
            }
        }
    }

    protected void submitTask(PriorityLeaderTaskExecutor executor, LoadTask task) throws LoadException {
        int retryNum = 0;
        while (!executor.submit(task)) {
            LOG.warn("submit load task failed. try to resubmit. job id: {}, task id: {}, retry: {}",
                    id, task.getSignature(), retryNum);
            if (++retryNum > TASK_SUBMIT_RETRY_NUM) {
                throw new LoadException("submit load task failed");
            }

            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                LOG.warn("Failed to execute sleep", e);
            }
        }
    }

    public void processTimeout() {
        // this is only for jobs which transaction is not started.
        // if transaction is started, global transaction manager will handle the timeout.
        writeLock();
        try {
            if (state != JobState.PENDING) {
                return;
            }

            if (!isTimeout()) {
                return;
            }

            unprotectedExecuteCancel(new FailMsg(FailMsg.CancelType.TIMEOUT, "loading timeout to cancel"), false);
            logFinalOperation();
        } finally {
            writeUnlock();
        }
    }

    protected void unprotectedExecuteJob() throws LoadException {
    }

    /**
     * This method only support update state to finished and loading.
     * It will not be persisted when desired state is finished because txn visible will edit the log.
     * If you want to update state to cancelled, please use the cancelJob function.
     *
     * @param jobState
     */
    public void updateState(JobState jobState) {
        writeLock();
        try {
            unprotectedUpdateState(jobState);
        } finally {
            writeUnlock();
        }
    }

    protected void unprotectedUpdateState(JobState jobState) {
        switch (jobState) {
            case UNKNOWN:
                executeUnknown();
                break;
            case LOADING:
                executeLoad();
                break;
            case COMMITTED:
                executeCommitted();
                break;
            case FINISHED:
                executeFinish();
                break;
            default:
                break;
        }
    }

    private void executeUnknown() {
        // set finished timestamp to create timestamp, so that this unknown job
        // can be remove due to label expiration so soon as possible
        finishTimestamp = createTimestamp;
        state = JobState.UNKNOWN;
    }

    private void executeLoad() {
        state = JobState.LOADING;
    }

    private void executeCommitted() {
        state = JobState.COMMITTED;
    }

    // if needLog is false, no need to write edit log.
    public void cancelJobWithoutCheck(FailMsg failMsg, boolean abortTxn, boolean needLog) {
        writeLock();
        try {
            unprotectedExecuteCancel(failMsg, abortTxn);
            if (needLog) {
                logFinalOperation();
            }
        } finally {
            writeUnlock();
        }
    }

    public void cancelJob(FailMsg failMsg) throws DdlException {
        writeLock();
        try {
            // mini load can not be cancelled by frontend
            if (jobType == EtlJobType.MINI) {
                throw new DdlException("Job could not be cancelled in type " + jobType.name());
            }
            if (isCommitting) {
                LOG.warn(new LogBuilder(LogKey.LOAD_JOB, id)
                        .add("error_msg", "The txn which belongs to job is committing. "
                                + "The job could not be cancelled in this step").build());
                throw new DdlException("Job could not be cancelled while txn is committing");
            }
            if (isTxnDone()) {
                LOG.warn(new LogBuilder(LogKey.LOAD_JOB, id)
                        .add("state", state)
                        .add("error_msg", "Job could not be cancelled when job is " + state)
                        .build());
                throw new DdlException("Job could not be cancelled when job is finished or cancelled");
            }

            unprotectedExecuteCancel(failMsg, true);
            logFinalOperation();
        } finally {
            writeUnlock();
        }
    }

    public void alterJob(AlterLoadStmt stmt) throws DdlException {
    }

    public void replayAlterJob(AlterLoadJobOperationLog log) {
    }

    /**
     * This method will cancel job without edit log and lock
     *
     * @param failMsg
     * @param abortTxn true: abort txn when cancel job, false: only change the state of job and ignore abort txn
     */
    protected void unprotectedExecuteCancel(FailMsg failMsg, boolean abortTxn) {
        LOG.warn(new LogBuilder(LogKey.LOAD_JOB, id).add("transaction_id", transactionId)
                .add("error_msg", "Failed to execute load with error: " + failMsg.getMsg()).build());

        // clean the loadingStatus
        loadingStatus.setState(TEtlState.CANCELLED);

        // get load ids of all loading tasks, we will cancel their coordinator process later
        List<TUniqueId> loadIds = Lists.newArrayList();
        for (PriorityLeaderTask loadTask : idToTasks.values()) {
            if (loadTask instanceof LoadLoadingTask) {
                loadIds.add(((LoadLoadingTask) loadTask).getLoadId());
            }
        }
        idToTasks.clear();

        // set failMsg and state
        this.failMsg = failMsg;
        if (failMsg.getCancelType() == CancelType.TXN_UNKNOWN) {
            // for bug fix, see LoadManager's fixLoadJobMetaBugs() method
            finishTimestamp = createTimestamp;
        } else {
            finishTimestamp = System.currentTimeMillis();
        }

        // remove callback before abortTransaction(), so that the afterAborted() callback will not be called again
        GlobalStateMgr.getCurrentState().getGlobalTransactionMgr().getCallbackFactory().removeCallback(id);

        if (abortTxn) {
            // abort txn
            try {
                LOG.debug(new LogBuilder(LogKey.LOAD_JOB, id)
                        .add("transaction_id", transactionId)
                        .add("msg", "begin to abort txn")
                        .build());
                GlobalStateMgr.getCurrentState().getGlobalTransactionMgr()
                        .abortTransaction(dbId, transactionId, failMsg.getMsg(),
                                getTabletCommitInfos(), getTabletFailInfos(), null);
            } catch (StarRocksException e) {
                LOG.warn(new LogBuilder(LogKey.LOAD_JOB, id)
                        .add("transaction_id", transactionId)
                        .add("error_msg", "failed to abort txn when job is cancelled. " + e.getMessage())
                        .build());
            }
        }

        // cancel all running coordinators, so that the scheduler's worker thread will be released
        for (TUniqueId loadId : loadIds) {
            Coordinator coordinator = QeProcessorImpl.INSTANCE.getCoordinator(loadId);
            if (coordinator != null) {
                coordinator.cancel(failMsg.getMsg());
            }
        }

        // change state
        state = JobState.CANCELLED;
    }

    private void executeFinish() {
        progress = 100;
        finishTimestamp = System.currentTimeMillis();
        GlobalStateMgr.getCurrentState().getGlobalTransactionMgr().getCallbackFactory().removeCallback(id);
        state = JobState.FINISHED;
        failMsg = null;

        if (MetricRepo.hasInit) {
            MetricRepo.COUNTER_LOAD_FINISHED.increase(1L);
        }
        // when load job finished, there is no need to hold the tasks which are the biggest memory consumers.
        idToTasks.clear();
    }

    public boolean checkDataQuality() {
        Map<String, String> counters = loadingStatus.getCounters();
        if (!counters.containsKey(DPP_NORMAL_ALL) || !counters.containsKey(DPP_ABNORMAL_ALL)) {
            return true;
        }

        long normalNum = Long.parseLong(counters.get(DPP_NORMAL_ALL));
        long abnormalNum = Long.parseLong(counters.get(DPP_ABNORMAL_ALL));
        if (abnormalNum > (abnormalNum + normalNum) * maxFilterRatio) {
            return false;
        }

        return true;
    }

    protected void logFinalOperation() {
        GlobalStateMgr.getCurrentState().getEditLog().logEndLoadJob(
                new LoadJobFinalOperation(id, loadingStatus, progress, loadStartTimestamp, finishTimestamp,
                        state, failMsg));
    }

    public void unprotectReadEndOperation(LoadJobFinalOperation loadJobFinalOperation, boolean isReplay) {
        loadingStatus = loadJobFinalOperation.getLoadingStatus();
        progress = loadJobFinalOperation.getProgress();
        loadStartTimestamp = loadJobFinalOperation.getLoadStartTimestamp();
        finishTimestamp = loadJobFinalOperation.getFinishTimestamp();
        if (isReplay) {
            state = loadJobFinalOperation.getJobState();
        }
        failMsg = loadJobFinalOperation.getFailMsg();
    }

    public void unprotectUpdateLoadingStatus(TransactionState txnState) {
        if (txnState.getTxnCommitAttachment() == null) {
            // The txn attachment maybe null when broker load has been cancelled without attachment.
            // The end log of broker load has been record but the callback id of txnState hasn't been removed
            // So the callback of txn is executed when log of txn aborted is replayed.
            return;
        }
        unprotectReadEndOperation((LoadJobFinalOperation) txnState.getTxnCommitAttachment(), false);
    }

    /**
     * This method will update job failMsg without edit log and lock
     *
     * @param failMsg
     */
    public void unprotectUpdateFailMsg(FailMsg failMsg) {
        this.failMsg = failMsg;
    }

    public List<Comparable> getShowInfo() throws DdlException {
        readLock();
        try {
            List<Comparable> jobInfo = Lists.newArrayList();
            // jobId
            jobInfo.add(id);
            // label
            jobInfo.add(label);
            // state
            if (state == JobState.COMMITTED) {
                jobInfo.add("PREPARED");
            } else if (state == JobState.LOADING && !startLoad) {
                jobInfo.add("QUEUEING");
            } else {
                jobInfo.add(state.name());
            }
            // progress
            switch (state) {
                case PENDING:
                    jobInfo.add("ETL:0%; LOAD:0%");
                    break;
                case CANCELLED:
                    jobInfo.add("ETL:N/A; LOAD:N/A");
                    break;
                case ETL:
                    jobInfo.add("ETL:" + progress + "%; LOAD:0%");
                    break;
                default:
                    jobInfo.add("ETL:100%; LOAD:" + progress + "%");
                    break;
            }

            // type
            jobInfo.add(jobType);
            // priority
            jobInfo.add(LoadPriority.priorityToName(priority));

            jobInfo.add(loadingStatus.getLoadStatistic().totalSourceLoadRows());
            jobInfo.add(loadingStatus.getLoadStatistic().totalFilteredRows());
            jobInfo.add(loadingStatus.getLoadStatistic().totalUnselectedRows());
            jobInfo.add(loadingStatus.getLoadStatistic().totalSinkLoadRows());

            // etl info
            if (jobType != EtlJobType.SPARK || loadingStatus.getCounters().size() == 0) {
                jobInfo.add(FeConstants.NULL_STRING);
            } else {
                jobInfo.add(Joiner.on("; ").withKeyValueSeparator("=").join(loadingStatus.getCounters()));
            }

            // task info
            jobInfo.add("resource:" + getResourceName() + "; timeout(s):" + timeoutSecond
                    + "; max_filter_ratio:" + maxFilterRatio);

            // error msg
            if (failMsg == null) {
                jobInfo.add(FeConstants.NULL_STRING);
            } else {
                jobInfo.add("type:" + failMsg.getCancelType() + "; msg:" + failMsg.getMsg());
            }

            // create time
            jobInfo.add(TimeUtils.longToTimeString(createTimestamp));
            // etl start time
            jobInfo.add(TimeUtils.longToTimeString(getEtlStartTimestamp()));
            // etl end time
            jobInfo.add(TimeUtils.longToTimeString(loadStartTimestamp));
            // load start time
            jobInfo.add(TimeUtils.longToTimeString(loadStartTimestamp));
            // load end time
            jobInfo.add(TimeUtils.longToTimeString(finishTimestamp));
            // tracking sql
            if (!loadingStatus.getTrackingUrl().equals(EtlStatus.DEFAULT_TRACKING_URL)) {
                jobInfo.add("select tracking_log from information_schema.load_tracking_logs where job_id=" + id);
            } else {
                jobInfo.add("");
            }
            jobInfo.add(loadingStatus.getLoadStatistic().toShowInfoStr());
            // warehouse
            if (RunMode.isSharedDataMode()) {
                Warehouse warehouse = GlobalStateMgr.getCurrentState().getWarehouseMgr().getWarehouse(warehouseId);
                jobInfo.add(warehouse.getName());
            } else {
                jobInfo.add("");
            }
            return jobInfo;
        } finally {
            readUnlock();
        }
    }

    public String toRuntimeDetails() {
        TreeMap<String, Object> runtimeDetails = Maps.newTreeMap();
        if (jobType == EtlJobType.SPARK) {
            if (loadingStatus.getCounters().size() != 0) {
                runtimeDetails.put(LoadConstants.RUNTIME_DETAILS_ETL_INFO,
                        Joiner.on("; ").withKeyValueSeparator("=").join(loadingStatus.getCounters()));
            }
            if (getEtlStartTimestamp() != -1) {
                runtimeDetails.put(LoadConstants.RUNTIME_DETAILS_ETL_START_TIME,
                        TimeUtils.longToTimeString(getEtlStartTimestamp()));
            }
            if (loadStartTimestamp != -1) {
                // etl end time
                runtimeDetails.put(LoadConstants.RUNTIME_DETAILS_ETL_FINISH_TIME,
                        TimeUtils.longToTimeString(loadStartTimestamp));
            }
        }
        runtimeDetails.put(LoadConstants.RUNTIME_DETAILS_LOAD_ID, Joiner.on(", ").join(loadIds));
        runtimeDetails.put(LoadConstants.RUNTIME_DETAILS_TXN_ID, transactionId);
        runtimeDetails.putAll(loadingStatus.getLoadStatistic().toRuntimeDetails());
        Gson gson = new Gson();
        return gson.toJson(runtimeDetails);
    }

    public String toProperties() {
        TreeMap<String, Object> properties = Maps.newTreeMap();
        properties.put(LoadConstants.PROPERTIES_TIMEOUT, timeoutSecond);
        properties.put(LoadConstants.PROPERTIES_MAX_FILTER_RATIO, maxFilterRatio);
        Gson gson = new Gson();
        return gson.toJson(properties);
    }

    public TLoadInfo toThrift() {
        readLock();
        try {
            TLoadInfo info = new TLoadInfo();
            info.setJob_id(id);
            info.setLabel(label);
            try {
                info.setDb(getDb().getFullName());
            } catch (MetaNotFoundException e) {
                info.setDb("");
            }
            info.setTxn_id(transactionId);
            if (!loadIds.isEmpty()) {
                info.setLoad_id(Joiner.on(", ").join(loadIds));

                List<String> profileIds = Lists.newArrayList();
                for (String loadId : loadIds) {
                    if (ProfileManager.getInstance().hasProfile(loadId)) {
                        profileIds.add(loadId);
                    }
                }
                if (!profileIds.isEmpty()) {
                    info.setProfile_id(Joiner.on(", ").join(loadIds));
                }
            }
            try {
                info.setTable(getTableNames(true).stream().collect(Collectors.joining(",")));
            } catch (MetaNotFoundException e) {
                info.setTable("");
            }
            info.setUser(user);

            if (state == JobState.COMMITTED) {
                info.setState("PREPARED");
            } else if (state == JobState.LOADING && !startLoad) {
                info.setState("QUEUEING");
            } else {
                info.setState(state.name());
            }
            // progress
            switch (state) {
                case PENDING:
                    info.setProgress("ETL:0%; LOAD:0%");
                    break;
                case CANCELLED:
                    info.setProgress("ETL:N/A; LOAD:N/A");
                    break;
                case ETL:
                    info.setProgress("ETL:" + progress + "%; LOAD:0%");
                    break;
                default:
                    info.setProgress("ETL:100%; LOAD:" + progress + "%");
                    break;
            }

            // type
            info.setType(jobType.name());
            // priority
            info.setPriority(LoadPriority.priorityToName(priority));

            // etl info
            if (jobType != EtlJobType.SPARK || loadingStatus.getCounters().size() == 0) {
                info.setEtl_info("");
            } else {
                info.setEtl_info(Joiner.on("; ").withKeyValueSeparator("=").join(loadingStatus.getCounters()));
            }

            // task info
            info.setRuntime_details(toRuntimeDetails());

            // error msg
            if (failMsg != null) {
                info.setError_msg("type:" + failMsg.getCancelType() + "; msg:" + failMsg.getMsg());
            }

            // create time
            if (createTimestamp != -1) {
                info.setCreate_time(TimeUtils.longToTimeString(createTimestamp));
            }
            // etl start time
            if (getEtlStartTimestamp() != -1) {
                info.setEtl_start_time(TimeUtils.longToTimeString(getEtlStartTimestamp()));
            }
            if (loadStartTimestamp != -1) {
                // etl end time
                info.setEtl_finish_time(TimeUtils.longToTimeString(loadStartTimestamp));
                // load start time
                info.setLoad_start_time(TimeUtils.longToTimeString(loadStartTimestamp));
            }
            if (loadCommittedTimestamp != -1) {
                info.setLoad_commit_time(TimeUtils.longToTimeString(loadCommittedTimestamp));
            }
            // load end time
            if (finishTimestamp != -1) {
                info.setLoad_finish_time(TimeUtils.longToTimeString(finishTimestamp));
            }
            // tracking url
            if (!loadingStatus.getTrackingUrl().equals(EtlStatus.DEFAULT_TRACKING_URL)) {
                info.setUrl(loadingStatus.getTrackingUrl());
                info.setTracking_sql("select tracking_log from information_schema.load_tracking_logs where job_id=" + id);
            }
            if (!loadingStatus.getRejectedRecordPaths().isEmpty()) {
                info.setRejected_record_path(Joiner.on(", ").join(loadingStatus.getRejectedRecordPaths()));
            }
            info.setProperties(toProperties());
            info.setNum_filtered_rows(loadingStatus.getLoadStatistic().totalFilteredRows());
            info.setNum_unselected_rows(loadingStatus.getLoadStatistic().totalUnselectedRows());
            info.setNum_scan_rows(loadingStatus.getLoadStatistic().totalSourceLoadRows());
            info.setNum_sink_rows(loadingStatus.getLoadStatistic().totalSinkLoadRows());
            info.setNum_scan_bytes(loadingStatus.getLoadStatistic().sourceScanBytes());
            // warehouse
            if (RunMode.getCurrentRunMode() == RunMode.SHARED_DATA) {
                Warehouse warehouse = GlobalStateMgr.getCurrentState().getWarehouseMgr().getWarehouse(warehouseId);
                info.setWarehouse(warehouse.getName());
            } else {
                info.setWarehouse("");
            }
            return info;
        } finally {
            readUnlock();
        }
    }

    public String getResourceName() {
        return "N/A";
    }

    protected long getEtlStartTimestamp() {
        return loadStartTimestamp;
    }

    public void getJobInfo(Load.JobInfo jobInfo) {
        jobInfo.tblNames.addAll(getTableNamesForShow());
        jobInfo.state = com.starrocks.load.loadv2.JobState.valueOf(state.name());
        if (failMsg != null) {
            jobInfo.failMsg = failMsg.getMsg();
        } else {
            jobInfo.failMsg = "";
        }
        jobInfo.trackingUrl = loadingStatus.getTrackingUrl();
    }

    @Override
    public long getCallbackId() {
        return id;
    }

    @Override
    public void beforeCommitted(TransactionState txnState) throws TransactionException {
        writeLock();
        try {
            if (isTxnDone()) {
                throw new TransactionException("txn could not be committed because job is: " + state);
            }
            isCommitting = true;
        } finally {
            writeUnlock();
        }
    }

    @Override
    public void afterCommitted(TransactionState txnState, boolean txnOperated) throws StarRocksException {
        if (!txnOperated) {
            return;
        }
        writeLock();
        try {
            unprotectUpdateLoadingStatus(txnState);
            isCommitting = false;
            state = JobState.COMMITTED;
            loadCommittedTimestamp = System.currentTimeMillis();
        } finally {
            writeUnlock();
        }
    }

    @Override
    public void replayOnCommitted(TransactionState txnState) {
        writeLock();
        try {
            replayTxnAttachment(txnState);
            progress = 99;
            transactionId = txnState.getTransactionId();
            state = JobState.COMMITTED;
            failMsg = null;
        } finally {
            writeUnlock();
        }
    }

    /**
     * This method will cancel job without edit log.
     * The job will be cancelled by replayOnAborted when journal replay
     *
     * @param txnState
     * @param txnOperated
     * @param txnStatusChangeReason
     */
    @Override
    public void afterAborted(TransactionState txnState, boolean txnOperated, String txnStatusChangeReason) {
        if (!txnOperated) {
            return;
        }
        writeLock();
        try {
            if (isTxnDone()) {
                return;
            }
            // record attachment in load job
            unprotectUpdateLoadingStatus(txnState);
            // cancel load job
            unprotectedExecuteCancel(new FailMsg(FailMsg.CancelType.LOAD_RUN_FAIL, txnStatusChangeReason), false);
        } finally {
            writeUnlock();
        }
    }

    /**
     * This method is used to replay the cancelled state of load job
     *
     * @param txnState
     */
    @Override
    public void replayOnAborted(TransactionState txnState) {
        writeLock();
        try {
            replayTxnAttachment(txnState);
            failMsg = new FailMsg(FailMsg.CancelType.LOAD_RUN_FAIL, txnState.getReason());
            finishTimestamp = txnState.getFinishTime();
            state = JobState.CANCELLED;
            GlobalStateMgr.getCurrentState().getGlobalTransactionMgr().getCallbackFactory().removeCallback(id);
        } finally {
            writeUnlock();
        }
    }

    /**
     * This method will finish the load job without edit log.
     * The job will be finished by replayOnVisible when txn journal replay
     *
     * @param txnState
     * @param txnOperated
     */
    @Override
    public void afterVisible(TransactionState txnState, boolean txnOperated) {
        if (!txnOperated) {
            return;
        }
        GlobalStateMgr.getCurrentState().getOperationListenerBus().onLoadJobTransactionFinish(txnState);
        unprotectUpdateLoadingStatus(txnState);
        updateState(JobState.FINISHED);
    }

    @Override
    public void replayOnVisible(TransactionState txnState) {
        writeLock();
        try {
            replayTxnAttachment(txnState);
            progress = 100;
            finishTimestamp = txnState.getFinishTime();
            state = JobState.FINISHED;
            failMsg = null;
            GlobalStateMgr.getCurrentState().getGlobalTransactionMgr().getCallbackFactory().removeCallback(id);
        } finally {
            writeUnlock();
        }
    }

    protected void replayTxnAttachment(TransactionState txnState) {
    }

    @Override
    public void onTaskFinished(TaskAttachment attachment) {
    }

    @Override
    public void onTaskFailed(long taskId, FailMsg failMsg, TaskAttachment attachment) {
    }

    // This analyze will be invoked after the replay is finished.
    // The edit log of LoadJob saves the origin param which is not analyzed.
    // So, the re-analyze must be invoked between the replay is finished and LoadJobScheduler is started.
    // Only, the PENDING load job need to be analyzed.
    public void analyze() {
    }

    @Override
    public int hashCode() {
        return Long.hashCode(id);
    }

    @Override
    public boolean equals(Object obj) {
        if (obj == this) {
            return true;
        }

        if (obj == null || getClass() != obj.getClass()) {
            return false;
        }

        LoadJob other = (LoadJob) obj;

        return this.id == other.id
                && this.dbId == other.dbId
                && this.label.equals(other.label)
                && this.state.equals(other.state)
                && this.jobType.equals(other.jobType);
    }



    public void replayUpdateStateInfo(LoadJobStateUpdateInfo info) {
        state = info.getState();
        transactionId = info.getTransactionId();
        loadStartTimestamp = info.getLoadStartTimestamp();
    }

    public boolean hasTxn() {
        return true;
    }

    public static class LoadJobStateUpdateInfo implements Writable {
        @SerializedName(value = "jobId")
        private long jobId;
        @SerializedName(value = "state")
        private JobState state;
        @SerializedName(value = "transactionId")
        private long transactionId;
        @SerializedName(value = "loadStartTimestamp")
        private long loadStartTimestamp;

        public LoadJobStateUpdateInfo(long jobId, JobState state, long transactionId, long loadStartTimestamp) {
            this.jobId = jobId;
            this.state = state;
            this.transactionId = transactionId;
            this.loadStartTimestamp = loadStartTimestamp;
        }

        public long getJobId() {
            return jobId;
        }

        public JobState getState() {
            return state;
        }

        public long getTransactionId() {
            return transactionId;
        }

        public long getLoadStartTimestamp() {
            return loadStartTimestamp;
        }

        @Override
        public String toString() {
            return GsonUtils.GSON.toJson(this);
        }

        @Override
        public void write(DataOutput out) throws IOException {
            String json = GsonUtils.GSON.toJson(this);
            Text.writeString(out, json);
        }

        public static LoadJobStateUpdateInfo read(DataInput in) throws IOException {
            String json = Text.readString(in);
            return GsonUtils.GSON.fromJson(json, LoadJobStateUpdateInfo.class);
        }
    }

    public static class JSONOptions {
        @SerializedName("s")
        public boolean stripOuterArray;

        @SerializedName("jp")
        public String jsonPaths;

        @SerializedName("jr")
        public String jsonRoot;
    }
}
