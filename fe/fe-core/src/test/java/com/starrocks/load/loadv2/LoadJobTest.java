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
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/test/java/org/apache/doris/load/loadv2/LoadJobTest.java

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
//

package com.starrocks.load.loadv2;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.DdlException;
import com.starrocks.common.DuplicatedRequestException;
import com.starrocks.common.LabelAlreadyUsedException;
import com.starrocks.common.LoadException;
import com.starrocks.common.MetaNotFoundException;
import com.starrocks.common.jmockit.Deencapsulation;
import com.starrocks.common.util.PropertyAnalyzer;
import com.starrocks.common.util.TimeUtils;
import com.starrocks.metric.LongCounterMetric;
import com.starrocks.metric.MetricRepo;
import com.starrocks.persist.EditLog;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.server.RunMode;
import com.starrocks.server.WarehouseManager;
import com.starrocks.sql.ast.LoadStmt;
import com.starrocks.task.LeaderTask;
import com.starrocks.task.LeaderTaskExecutor;
import com.starrocks.thrift.TLoadInfo;
import com.starrocks.thrift.TUniqueId;
import com.starrocks.transaction.GlobalTransactionMgr;
import com.starrocks.transaction.RunningTxnExceedException;
import com.starrocks.transaction.TransactionState;
import com.starrocks.warehouse.Warehouse;
import com.starrocks.warehouse.cngroup.ComputeResource;
import mockit.Expectations;
import mockit.Injectable;
import mockit.Mock;
import mockit.MockUp;
import mockit.Mocked;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import java.time.ZoneId;
import java.util.List;
import java.util.Map;
import java.util.TimeZone;
import java.util.concurrent.ArrayBlockingQueue;

public class LoadJobTest {

    @Mocked
    private GlobalStateMgr globalStateMgr;

    @Mocked
    private WarehouseManager warehouseManager;

    @Mocked
    private Warehouse warehouse;

    @BeforeAll
    public static void start() {
        MetricRepo.init();
    }

    @Test
    public void testGetDbNotExists(@Mocked GlobalStateMgr globalStateMgr) {
        LoadJob loadJob = new BrokerLoadJob();
        Deencapsulation.setField(loadJob, "dbId", 1L);
        new Expectations() {
            {
                globalStateMgr.getLocalMetastore().getDb(1L);
                minTimes = 0;
                result = null;
            }
        };

        try {
            loadJob.getDb();
            Assertions.fail();
        } catch (MetaNotFoundException e) {
        }
    }

    @Test
    public void testSetJobPropertiesWithErrorTimeout() {
        Map<String, String> jobProperties = Maps.newHashMap();
        jobProperties.put(LoadStmt.TIMEOUT_PROPERTY, "abc");
        LoadJob loadJob = new BrokerLoadJob();
        try {
            loadJob.setJobProperties(jobProperties);
            Assertions.fail();
        } catch (DdlException e) {
        }
    }

    @Test
    public void testSetJobProperties() {
        new Expectations() {
            {
                GlobalStateMgr.getCurrentState();
                minTimes = 0;
                result = globalStateMgr;

                globalStateMgr.getWarehouseMgr();
                minTimes = 0;
                result = warehouseManager;

                warehouseManager.getWarehouse(anyLong);
                minTimes = 0;
                result = warehouse;

                warehouse.getId();
                minTimes = 0;
                result = 1001L;
            }
        };

        Map<String, String> jobProperties = Maps.newHashMap();
        jobProperties.put(LoadStmt.TIMEOUT_PROPERTY, "1000");
        jobProperties.put(LoadStmt.MAX_FILTER_RATIO_PROPERTY, "0.1");
        jobProperties.put(LoadStmt.LOAD_MEM_LIMIT, "1024");
        jobProperties.put(LoadStmt.STRICT_MODE, "True");

        LoadJob loadJob = new BrokerLoadJob();
        try {
            loadJob.setJobProperties(jobProperties);
            Assertions.assertEquals(1000, (long) Deencapsulation.getField(loadJob, "timeoutSecond"));
            Assertions.assertEquals(0.1, Deencapsulation.getField(loadJob, "maxFilterRatio"), 0);
            Assertions.assertEquals(1024, (long) Deencapsulation.getField(loadJob, "loadMemLimit"));
            Assertions.assertTrue((Boolean) Deencapsulation.getField(loadJob, "strictMode"));
        } catch (DdlException e) {
            Assertions.fail(e.getMessage());
        }
    }

    @Test
    public void testSetJobPropertiesForWarehouse() {
        new MockUp<RunMode>() {
            @Mock
            public RunMode getCurrentRunMode() {
                return RunMode.SHARED_DATA;
            }
        };

        new Expectations() {
            {
                GlobalStateMgr.getCurrentState();
                minTimes = 0;
                result = globalStateMgr;

                globalStateMgr.getWarehouseMgr();
                minTimes = 0;
                result = warehouseManager;

                warehouseManager.getWarehouse(anyLong);
                minTimes = 0;
                result = warehouse;

                warehouse.getId();
                minTimes = 0;
                result = 1001L;
            }
        };

        ConnectContext context = new ConnectContext(null);
        new Expectations(context) {
            {
                ConnectContext.get();
                result = context;

                context.getCurrentWarehouseId();
                result = 1000L;
            }
        };

        try {
            // normal, jobProperties set
            LoadJob loadJob1 = new BrokerLoadJob();
            Map<String, String> jobProperties1 = Maps.newHashMap();
            jobProperties1.put(PropertyAnalyzer.PROPERTIES_WAREHOUSE, "test_warehouse");
            loadJob1.setJobProperties(jobProperties1);
            Assertions.assertEquals(1001L, (long) Deencapsulation.getField(loadJob1, "warehouseId"));

            // with jobProperties set, but no warehouse property,
            LoadJob loadJob2 = new BrokerLoadJob();
            Map<String, String> jobProperties2 = Maps.newHashMap();
            loadJob2.setJobProperties(jobProperties2);
            Assertions.assertEquals(1000L, (long) Deencapsulation.getField(loadJob2, "warehouseId"));

            // no jobProperties provided
            LoadJob loadJob3 = new BrokerLoadJob();
            loadJob3.setJobProperties(null);
            Assertions.assertEquals(1000L, (long) Deencapsulation.getField(loadJob3, "warehouseId"));
        } catch (DdlException e) {
            Assertions.fail(e.getMessage());
        }
    }

    @Test
    public void testExecute(@Mocked GlobalTransactionMgr globalTransactionMgr,
                            @Mocked LeaderTaskExecutor leaderTaskExecutor)
            throws LabelAlreadyUsedException, RunningTxnExceedException, AnalysisException, DuplicatedRequestException {
        LoadJob loadJob = new BrokerLoadJob();
        new Expectations() {
            {
                globalTransactionMgr.beginTransaction(anyLong, Lists.newArrayList(), anyString, (TUniqueId) any,
                        (TransactionState.TxnCoordinator) any,
                        (TransactionState.LoadJobSourceType) any, anyLong, anyLong, (ComputeResource) any);
                minTimes = 0;
                result = 1;
                leaderTaskExecutor.submit((LeaderTask) any);
                minTimes = 0;
                result = true;
            }
        };

        GlobalStateMgr.getCurrentState().setEditLog(new EditLog(new ArrayBlockingQueue<>(100)));
        new MockUp<EditLog>() {
            @Mock
            public void logSaveNextId(long nextId) {

            }
        };

        try {
            loadJob.execute();
        } catch (LoadException e) {
            Assertions.fail(e.getMessage());
        }
        Assertions.assertEquals(JobState.LOADING, loadJob.getState());
        Assertions.assertEquals(1, loadJob.getTransactionId());

    }

    @Test
    public void testProcessTimeoutWithCompleted() {
        LoadJob loadJob = new BrokerLoadJob();
        Deencapsulation.setField(loadJob, "state", JobState.FINISHED);

        loadJob.processTimeout();
        Assertions.assertEquals(JobState.FINISHED, loadJob.getState());
    }

    @Test
    public void testProcessTimeoutWithIsCommitting() {
        LoadJob loadJob = new BrokerLoadJob();
        Deencapsulation.setField(loadJob, "isCommitting", true);
        Deencapsulation.setField(loadJob, "state", JobState.LOADING);

        loadJob.processTimeout();
        Assertions.assertEquals(JobState.LOADING, loadJob.getState());
    }

    @Test
    public void testProcessTimeoutWithLongTimeoutSecond() {
        LoadJob loadJob = new BrokerLoadJob();
        Deencapsulation.setField(loadJob, "createTimestamp", System.currentTimeMillis());
        Deencapsulation.setField(loadJob, "timeoutSecond", 1000L);

        loadJob.processTimeout();
        Assertions.assertEquals(JobState.PENDING, loadJob.getState());
    }

    @Test
    public void testProcessTimeout(@Mocked GlobalStateMgr globalStateMgr, @Mocked EditLog editLog) {
        LoadJob loadJob = new BrokerLoadJob();
        Deencapsulation.setField(loadJob, "timeoutSecond", 0);
        new Expectations() {
            {
                globalStateMgr.getEditLog();
                minTimes = 0;
                result = editLog;
            }
        };

        loadJob.processTimeout();
        Assertions.assertEquals(JobState.CANCELLED, loadJob.getState());
    }

    @Test
    public void testUpdateStateToLoading() {
        LoadJob loadJob = new BrokerLoadJob();
        loadJob.updateState(JobState.LOADING);
        Assertions.assertEquals(JobState.LOADING, loadJob.getState());
    }

    @Test
    public void testUpdateStateToFinished(@Mocked MetricRepo metricRepo,
                                          @Injectable LoadTask loadTask1,
                                          @Mocked LongCounterMetric longCounterMetric) {

        MetricRepo.COUNTER_LOAD_FINISHED = longCounterMetric;
        LoadJob loadJob = new BrokerLoadJob();
        loadJob.idToTasks.put(1L, loadTask1);

        // TxnStateCallbackFactory factory = GlobalStateMgr.getCurrentState().getGlobalTransactionMgr().getCallbackFactory();
        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        GlobalTransactionMgr mgr = new GlobalTransactionMgr(globalStateMgr);
        Deencapsulation.setField(globalStateMgr, "globalTransactionMgr", mgr);
        Assertions.assertEquals(1, loadJob.idToTasks.size());
        loadJob.updateState(JobState.FINISHED);
        Assertions.assertEquals(JobState.FINISHED, loadJob.getState());
        Assertions.assertNotEquals(-1, (long) Deencapsulation.getField(loadJob, "finishTimestamp"));
        Assertions.assertEquals(100, (int) Deencapsulation.getField(loadJob, "progress"));
        Assertions.assertEquals(0, loadJob.idToTasks.size());
    }

    @Test
    public void testGetShowInfo() throws DdlException {
        TimeZone tz = TimeZone.getTimeZone(ZoneId.of("Asia/Shanghai"));
        new MockUp<TimeUtils>() {
            @Mock
            public TimeZone getTimeZone() {
                return tz;
            }
        };

        new MockUp<RunMode>() {
            @Mock
            public RunMode getCurrentRunMode() {
                return RunMode.SHARED_DATA;
            }
        };

        new Expectations() {
            {
                GlobalStateMgr.getCurrentState();
                minTimes = 0;
                result = globalStateMgr;

                globalStateMgr.getWarehouseMgr();
                minTimes = 0;
                result = warehouseManager;

                warehouseManager.getWarehouse(anyLong);
                minTimes = 0;
                result = warehouse;

                warehouse.getName();
                minTimes = 0;
                result = "test_wh";
            }
        };

        LoadJob loadJob = new BrokerLoadJob();
        List<Comparable> showInfo = loadJob.getShowInfo();
        Assertions.assertNotNull(showInfo);
        Comparable result = showInfo.get(showInfo.size() - 1);
        Assertions.assertEquals("test_wh", result);

        new MockUp<RunMode>() {
            @Mock
            public RunMode getCurrentRunMode() {
                return RunMode.SHARED_NOTHING;
            }
        };

        showInfo = loadJob.getShowInfo();
        Assertions.assertNotNull(showInfo);
        result = showInfo.get(showInfo.size() - 1);
        Assertions.assertEquals("", result);
    }

    @Test
    public void testToThrift() {
        LoadJob loadJob = new BrokerLoadJob();

        new MockUp<RunMode>() {
            @Mock
            public RunMode getCurrentRunMode() {
                return RunMode.SHARED_DATA;
            }
        };

        new Expectations() {
            {
                GlobalStateMgr.getCurrentState();
                minTimes = 0;
                result = globalStateMgr;

                globalStateMgr.getWarehouseMgr();
                minTimes = 0;
                result = warehouseManager;

                warehouseManager.getWarehouse(anyLong);
                minTimes = 0;
                result = warehouse;

                warehouse.getName();
                minTimes = 0;
                result = "test_wh";
            }
        };

        TLoadInfo loadInfo = loadJob.toThrift();
        Assertions.assertEquals("test_wh", loadInfo.getWarehouse());

        new MockUp<RunMode>() {
            @Mock
            public RunMode getCurrentRunMode() {
                return RunMode.SHARED_NOTHING;
            }
        };

        loadInfo = loadJob.toThrift();
        Assertions.assertEquals("", loadInfo.getWarehouse());
    }
}
