#!/usr/bin/env python3

import time
import json
import os
import shutil
import signal
import sys

from TestHarness import Cluster, TestHelper, Utils, WalletMgr
from TestHarness.TestHelper import AppArgs

###############################################################
# ship_streamer_test
# 
# This test sets up 4 producing nodes and one "bridge" node using test_control_api_plugin.
#   One side of bridge has 3 of the elected producers and the other has 1 of the elected producers.
#   All the producers are named in alphabetical order, so that the 3 producers, in the one production side, are
#       scheduled first, followed by the 1 producer in the other producer node. Each producing side is only connected
#       to the other producing node via the "bridge" node.
#   The bridge node has the test_control_api_plugin, that the test uses to kill
#       the "bridge" node to generate a fork.
#   ship_streamer is used to connect to the state_history_plugin and verify that blocks receive link to previous
#   blocks. If the blocks do not link then ship_streamer will exit with an error causing this test to generate an
#   error. The fork generated by nodeos should be sent to the ship_streamer so it is able to correctly observe the
#   fork.
#
###############################################################

Print=Utils.Print

appArgs = AppArgs()
extraArgs = appArgs.add(flag="--num-clients", type=int, help="How many ship_streamers should be started", default=1)
extraArgs = appArgs.add_bool(flag="--finality-data-history", help="Enable finality data history", action='store_true')
args = TestHelper.parse_args({"--activate-if","--dump-error-details","--keep-logs","-v","--leave-running","--unshared"}, applicationSpecificArgs=appArgs)

Utils.Debug=args.v
cluster=Cluster(unshared=args.unshared, keepRunning=args.leave_running, keepLogs=args.keep_logs)
activateIF=args.activate_if
dumpErrorDetails=args.dump_error_details
walletPort=TestHelper.DEFAULT_WALLET_PORT

totalProducerNodes=4
totalNonProducerNodes=1
totalNodes=totalProducerNodes+totalNonProducerNodes
maxActiveProducers=21
totalProducers=maxActiveProducers

walletMgr=WalletMgr(True, port=walletPort)
testSuccessful=False

WalletdName=Utils.EosWalletName
shipTempDir=None

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)
    Print("Stand up cluster")

    # ***   setup topogrophy   ***

    # "bridge" shape connects defproducera (node0) defproducerb (node1) defproducerc (node2) to each other and defproducerd (node3)
    # and the only connection between those 2 groups is through the bridge (node4)

    shipNodeNum = 3
    specificExtraNodeosArgs={}
    specificExtraNodeosArgs[shipNodeNum]="--plugin eosio::state_history_plugin --trace-history --chain-state-history --state-history-stride 200 --plugin eosio::net_api_plugin --plugin eosio::producer_api_plugin "
    if args.finality_data_history:
        specificExtraNodeosArgs[shipNodeNum]+=" --finality-data-history"
    # producer nodes will be mapped to 0 through totalProducerNodes-1, so the number totalProducerNodes will be the non-producing node
    specificExtraNodeosArgs[totalProducerNodes]="--plugin eosio::test_control_api_plugin  "

    if cluster.launch(topo="./tests/bridge_for_fork_test_shape.json", pnodes=totalProducerNodes, loadSystemContract=False,
                      totalNodes=totalNodes, totalProducers=totalProducerNodes, activateIF=activateIF, biosFinalizer=False,
                      specificExtraNodeosArgs=specificExtraNodeosArgs) is False:
        Utils.cmdError("launcher")
        Utils.errorExit("Failed to stand up cluster.")

    # ***   identify each node (producers and non-producing node)   ***

    # verify nodes are in sync and advancing
    cluster.waitOnClusterSync(blockAdvancing=5)
    Print("Cluster in Sync")

    prodNode0 = cluster.getNode(0)
    prodNode3 = cluster.getNode(3)
    nonProdNode = cluster.getNode(4)
    shipNode = cluster.getNode(shipNodeNum)

    # cluster.waitOnClusterSync(blockAdvancing=3)
    start_block_num = shipNode.getBlockNum()

    #verify nodes are in sync and advancing
    cluster.waitOnClusterSync(blockAdvancing=3)
    Print("Shutdown unneeded bios node")
    cluster.biosNode.kill(signal.SIGTERM)

    Print("Create a jumbo row")
    contract = "jumborow"
    contractDir = "unittests/contracts/%s" % (contract)
    wasmFile = "%s.wasm" % (contract)
    abiFile = "%s.abi" % (contract)

    nonProdNode.publishContract(cluster.defproducerbAccount, contractDir, wasmFile, abiFile)
    jumbotxn = {

        "actions": [{"account": "defproducerb","name": "jumbotime",
                     "authorization": [{"actor": "defproducerb","permission": "active"}],
                     "data": "",
                     "compression": "none"}]
    }
    nonProdNode.pushTransaction(jumbotxn)

    Print("Configure and launch txn generators")
    targetTpsPerGenerator = 10
    testTrxGenDurationSec=60*60
    numTrxGenerators=2
    cluster.launchTrxGenerators(contractOwnerAcctName=cluster.eosioAccount.name, acctNamesList=[cluster.defproduceraAccount.name, cluster.defproducerbAccount.name],
                                acctPrivKeysList=[cluster.defproduceraAccount.activePrivateKey,cluster.defproducerbAccount.activePrivateKey], nodeId=prodNode3.nodeId,
                                tpsPerGenerator=targetTpsPerGenerator, numGenerators=numTrxGenerators, durationSec=testTrxGenDurationSec,
                                waitToComplete=False)

    status = cluster.waitForTrxGeneratorsSpinup(nodeId=prodNode3.nodeId, numGenerators=numTrxGenerators)
    assert status is not None and status is not False, "ERROR: Failed to spinup Transaction Generators"

    prodNode0.waitForProducer("defproducerc")

    block_range = 250
    end_block_num = start_block_num + block_range

    shipClient = "tests/ship_streamer"
    cmd = f"{shipClient} --start-block-num {start_block_num} --end-block-num {end_block_num} --fetch-block --fetch-traces --fetch-deltas"
    if args.finality_data_history:
        cmd += "  --fetch-finality-data"
    if Utils.Debug: Utils.Print(f"cmd: {cmd}")
    clients = []
    files = []
    shipTempDir = os.path.join(Utils.DataDir, "ship")
    os.makedirs(shipTempDir, exist_ok = True)
    shipClientFilePrefix = os.path.join(shipTempDir, "client")

    starts = []
    for i in range(0, args.num_clients):
        start = time.perf_counter()
        outFile = open(f"{shipClientFilePrefix}{i}.out", "w")
        errFile = open(f"{shipClientFilePrefix}{i}.err", "w")
        Print(f"Start client {i}")
        popen=Utils.delayedCheckOutput(cmd, stdout=outFile, stderr=errFile)
        starts.append(time.perf_counter())
        clients.append((popen, cmd))
        files.append((outFile, errFile))
        Print(f"Client {i} started, Ship node head is: {shipNode.getBlockNum()}")

    # Generate a fork
    prodNode3Prod= "defproducerd"
    preKillBlockNum=nonProdNode.getBlockNum()
    preKillBlockProducer=nonProdNode.getBlockProducerByNum(preKillBlockNum)
    forkAtProducer="defproducerb"
    nonProdNode.killNodeOnProducer(producer=forkAtProducer, whereInSequence=1)
    Print(f"Current block producer {preKillBlockProducer} fork will be at producer {forkAtProducer}")
    prodNode0.waitForProducer("defproducera")
    prodNode3.waitForProducer(prodNode3Prod)
    if nonProdNode.verifyAlive():
        prodNode0.waitForProducer("defproducera")
        prodNode3.waitForProducer(prodNode3Prod)
    if nonProdNode.verifyAlive():
        Utils.errorExit("Bridge did not shutdown")
    Print("Fork started")

    prodNode0.waitForProducer("defproducerc") # wait for fork to progress a bit
    restore0BlockNum = prodNode0.getBlockNum()
    restore1BlockNum = prodNode3.getBlockNum()
    restoreBlockNum = max(int(restore0BlockNum), int(restore1BlockNum))
    restore0LIB = prodNode0.getIrreversibleBlockNum()
    restore1LIB = prodNode3.getIrreversibleBlockNum()
    restoreLIB = max(int(restore0LIB), int(restore1LIB))

    if int(restoreBlockNum) > int(end_block_num):
        Utils.errorExit(f"Did not stream long enough {end_block_num} to cover the fork {restoreBlockNum}, increase block_range {block_range}")

    Print("Restore fork")
    Print("Relaunching the non-producing bridge node to connect the producing nodes again")
    if nonProdNode.verifyAlive():
        Utils.errorExit("Bridge is already running")
    if not nonProdNode.relaunch():
        Utils.errorExit(f"Failure - (non-production) node {nonProdNode.nodeNum} should have restarted")

    nonProdNode.waitForProducer(forkAtProducer)
    nonProdNode.waitForProducer(prodNode3Prod)
    nonProdNode.waitForIrreversibleBlock(restoreLIB+1)
    afterForkBlockNum = nonProdNode.getBlockNum()

    assert shipNode.findInLog(f"successfully switched fork to new head"), f"No fork found in log {shipNode}"

    Print(f"Stopping all {args.num_clients} clients")
    for index, (popen, _), (out, err), start in zip(range(len(clients)), clients, files, starts):
        popen.wait()
        Print(f"Stopped client {index}.  Ran for {time.perf_counter() - start:.3f} seconds.")
        out.close()
        err.close()
        outFile = open(f"{shipClientFilePrefix}{index}.out", "r")
        data = json.load(outFile)
        block_num = start_block_num
        for i in data:
            # fork can cause block numbers to be repeated
            this_block_num = i['get_blocks_result_v1']['this_block']['block_num']
            if this_block_num < block_num:
                block_num = this_block_num
            assert block_num == this_block_num, f"{block_num} != {this_block_num}"
            assert isinstance(i['get_blocks_result_v1']['block'], str) # verify block in result
            block_num += 1
        assert block_num-1 == end_block_num, f"{block_num-1} != {end_block_num}"

    Print("Generate snapshot")
    shipNode.createSnapshot()

    Print("Shutdown state_history_plugin nodeos")
    shipNode.kill(signal.SIGTERM)

    Print("Shutdown bridge node")
    nonProdNode.kill(signal.SIGTERM)

    Print("Test starting ship from snapshot")
    Utils.rmNodeDataDir(shipNodeNum)
    isRelaunchSuccess = shipNode.relaunch(chainArg=" --snapshot {}".format(shipNode.getLatestSnapshot()))
    assert isRelaunchSuccess, "relaunch from snapshot failed"

    afterSnapshotBlockNum = shipNode.getBlockNum()

    Print("Verify we can stream from ship after start from a snapshot with no incoming trxs")
    start_block_num = afterSnapshotBlockNum
    block_range = 0
    end_block_num = start_block_num + block_range
    cmd = f"{shipClient} --start-block-num {start_block_num} --end-block-num {end_block_num} --fetch-block --fetch-traces --fetch-deltas"
    if args.finality_data_history:
        cmd += "  --fetch-finality-data"
    if Utils.Debug: Utils.Print(f"cmd: {cmd}")
    clients = []
    files = []
    starts = []
    for i in range(0, args.num_clients):
        start = time.perf_counter()
        outFile = open(f"{shipClientFilePrefix}{i}_snapshot.out", "w")
        errFile = open(f"{shipClientFilePrefix}{i}_snapshot.err", "w")
        Print(f"Start client {i}")
        popen=Utils.delayedCheckOutput(cmd, stdout=outFile, stderr=errFile)
        starts.append(time.perf_counter())
        clients.append((popen, cmd))
        files.append((outFile, errFile))
        Print(f"Client {i} started, Ship node head is: {shipNode.getBlockNum()}")

    Print(f"Stopping all {args.num_clients} clients")
    for index, (popen, _), (out, err), start in zip(range(len(clients)), clients, files, starts):
        popen.wait()
        Print(f"Stopped client {index}.  Ran for {time.perf_counter() - start:.3f} seconds.")
        out.close()
        err.close()
        outFile = open(f"{shipClientFilePrefix}{index}_snapshot.out", "r")
        data = json.load(outFile)
        block_num = start_block_num
        for i in data:
            # fork can cause block numbers to be repeated
            this_block_num = i['get_blocks_result_v1']['this_block']['block_num']
            if this_block_num < block_num:
                block_num = this_block_num
            assert block_num == this_block_num, f"{block_num} != {this_block_num}"
            assert isinstance(i['get_blocks_result_v1']['deltas'], str) # verify deltas in result
            block_num += 1
        assert block_num-1 == end_block_num, f"{block_num-1} != {end_block_num}"

    testSuccessful = True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful=testSuccessful, dumpErrorDetails=dumpErrorDetails)
    if shipTempDir is not None:
        if testSuccessful and not args.keep_logs:
            shutil.rmtree(shipTempDir, ignore_errors=True)

errorCode = 0 if testSuccessful else 1
exit(errorCode)
