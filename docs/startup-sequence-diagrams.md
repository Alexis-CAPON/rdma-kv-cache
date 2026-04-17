# Startup Sequence Diagrams

This document provides detailed sequence diagrams for the orchestrator and prefill/decode node startup processes.

## 1. Orchestrator Startup Sequence

```
orchestrator_main.cpp → OrchestratorEngine Initialization & Startup

┌─────────────┐  ┌──────────────────┐  ┌────────────┐  ┌─────────────┐  ┌────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│orchestrator_│  │OrchestratorEngine│  │TCPServer   │  │EpollWorker  │  │WorkerPool  │  │NodeRegistry  │  │RequestRouter │  │RequestTracker│  │RdmaExchange  │
│main()       │  │                  │  │(client +   │  │(I/O thread) │  │(N workers) │  │              │  │              │  │              │  │Tracker       │
│             │  │                  │  │ server)    │  │             │  │            │  │              │  │              │  │              │  │              │
└──────┬──────┘  └────────┬─────────┘  └─────┬──────┘  └──────┬──────┘  └─────┬──────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                  │                   │                │                │               │                 │                 │                 │
       │ [1] Parse CLI args                  │                │                │               │                 │                 │                 │
       │ --config orchestrator.yaml          │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │ [2] Load config  │                   │                │                │               │                 │                 │                 │
       │ - client_socket_port: 8080          │                │                │               │                 │                 │                 │
       │ - server_socket_port: 9090          │                │                │               │                 │                 │                 │
       │ - worker_pool_size: 4               │                │                │               │                 │                 │                 │
       │ - expected_prefill_nodes: 2         │                │                │               │                 │                 │                 │
       │ - expected_decode_nodes: 3          │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │ [3] new OrchestratorEngine(config)  │                │                │               │                 │                 │                 │
       ├─────────────────>│                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │ CONSTRUCTOR PHASE - Initialize all components (orchestrator_engine.cpp:14-35)
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [4] Create EventQueue             │                │               │                 │                 │                 │
       │                  │ (config.event_queue_size)         │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [5] Create NodeRegistry           │                │               │                 │                 │                 │
       │                  ├─────────────────────────────────────────────────────────────────>│                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │ [6] Initialize  │                 │                 │
       │                  │                   │                │                │               │ - node_map_: unordered_map<node_id, NodeInfo>
       │                  │                   │                │                │               │ - health_status_: map<node_id, bool>
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [7] Create RequestRouter          │                │               │                 │                 │                 │
       │                  │ (node_registry_, LEAST_LOADED)    │                │               │                 │                 │                 │
       │                  ├───────────────────────────────────────────────────────────────────────────────────>│                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │ [8] Initialize │                 │
       │                  │                   │                │                │               │                 │ - policy_: LEAST_LOADED
       │                  │                   │                │                │               │                 │ - node_loads_: map<node_id, uint32_t>
       │                  │                   │                │                │               │                 │ - node_registry_ref
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [9] Create RequestTracker         │                │               │                 │                 │                 │
       │                  ├───────────────────────────────────────────────────────────────────────────────────────────────────────>│                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │ [10] Initialize│
       │                  │                   │                │                │               │                 │                 │ - requests_: map<request_id, TrackedRequest>
       │                  │                   │                │                │               │                 │                 │ - mutex for thread safety
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [11] Create RdmaExchangeTracker   │                │               │                 │                 │                 │
       │                  │ (expected_prefill: 2, expected_decode: 3)          │               │                 │                 │                 │
       │                  ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────>│
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │ [12] Initialize│
       │                  │                   │                │                │               │                 │                 │ - expected_prefill_nodes_: 2
       │                  │                   │                │                │               │                 │                 │ - expected_decode_nodes_: 3
       │                  │                   │                │                │               │                 │                 │ - registered_nodes_: set<node_id>
       │                  │                   │                │                │               │                 │                 │ - ready_nodes_: set<node_id>
       │                  │                   │                │                │               │                 │                 │ - state: WAITING_REGISTRATION
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [13] Create EpollWorker           │                │               │                 │                 │                 │
       │                  │ (event_queue_, client_tcp_server_, server_tcp_server_)             │                 │                 │                 │
       │                  ├─────────────────────────────────────────────────>│                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │ [14] Initialize│               │                 │                 │                 │
       │                  │                   │                │ - epoll_fd_ = epoll_create1()│               │                 │                 │                 │
       │                  │                   │                │ - client_tcp_server_ref      │               │                 │                 │                 │
       │                  │                   │                │ - server_tcp_server_ref      │               │                 │                 │                 │
       │                  │                   │                │ - event_queue_ref            │               │                 │                 │                 │
       │                  │                   │                │ - connections_: map<fd, Connection>          │                 │                 │                 │
       │                  │                   │                │ - node_id_to_fd_: map for routing           │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [15] Create WorkerPool            │                │               │                 │                 │                 │
       │                  │ (num_workers: 4, event_queue_, epoll_worker_, config_)            │                 │                 │                 │
       │                  ├─────────────────────────────────────────────────────────────────>│               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │ [16] Initialize               │                 │                 │
       │                  │                   │                │                │ - num_workers_: 4             │                 │                 │
       │                  │                   │                │                │ - event_queue_ref             │                 │                 │
       │                  │                   │                │                │ - epoll_worker_ref            │                 │                 │
       │                  │                   │                │                │ - workers_: vector<thread>    │                 │                 │
       │                  │                   │                │                │ - running_: false             │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [17] Constructor complete          │                │               │                 │                 │                 │
       │                  │ State = STARTING  │                │                │               │                 │                 │                 │
       │<─────────────────┤                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │ [18] Call orchestrator_engine.start()                │                │               │                 │                 │                 │
       ├─────────────────>│                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │ START PHASE - Bind sockets and start threads (orchestrator_engine.cpp:37-106)
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [19] LOG: "Starting orchestrator..." │              │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │ PHASE 1: TCP Server Binding (lines 45-75)
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [20] Bind client TCP server       │                │               │                 │                 │                 │
       │                  ├──────────────────>│                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │ [21] socket() -> fd=10         │               │                 │                 │                 │
       │                  │                   │ setsockopt(SO_REUSEADDR)       │               │                 │                 │                 │
       │                  │                   │ bind(0.0.0.0:8080)             │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [22] client_tcp_server_.listen()  │                │               │                 │                 │                 │
       │                  ├──────────────────>│                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │ [23] listen(fd=10, backlog=128)│               │                 │                 │                 │
       │                  │                   │ set_nonblocking(fd=10)         │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │<──────────────────┤                │                │               │                 │                 │                 │
       │                  │ SUCCESS: client_fd_ = 10           │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [24] LOG: "Client TCP listening on port 8080"      │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [25] Bind server TCP server       │                │               │                 │                 │                 │
       │                  ├──────────────────>│                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │ [26] socket() -> fd=11         │               │                 │                 │                 │
       │                  │                   │ setsockopt(SO_REUSEADDR)       │               │                 │                 │                 │
       │                  │                   │ bind(0.0.0.0:9090)             │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [27] server_tcp_server_.listen()  │                │               │                 │                 │                 │
       │                  ├──────────────────>│                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │ [28] listen(fd=11, backlog=128)│               │                 │                 │                 │
       │                  │                   │ set_nonblocking(fd=11)         │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │<──────────────────┤                │                │               │                 │                 │                 │
       │                  │ SUCCESS: server_fd_ = 11           │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [29] LOG: "Server TCP listening on port 9090"      │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │ PHASE 2: Start EpollWorker (lines 81-82)
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [30] Start EpollWorker            │                │               │                 │                 │                 │
       │                  ├─────────────────────────────────────────────────>│                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │ [31] Add client listen socket to epoll          │                 │                 │
       │                  │                   │                │ epoll_ctl(ADD, fd=10, EPOLLIN | EPOLLET)        │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │ [32] Add server listen socket to epoll          │                 │                 │
       │                  │                   │                │ epoll_ctl(ADD, fd=11, EPOLLIN | EPOLLET)        │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │ [33] Spawn I/O thread                           │                 │                 │
       │                  │                   │                │ running_ = true │               │                 │                 │                 │
       │                  │                   │                │ io_thread_ = std::thread(&EpollWorker::io_loop, this)             │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │ ╔══════════════════════════════════════════════════════════════╗  │                 │
       │                  │                   │                │ ║ I/O THREAD RUNNING IN BACKGROUND                            ║  │                 │
       │                  │                   │                │ ║ - epoll_wait() on fd=10, fd=11                              ║  │                 │
       │                  │                   │                │ ║ - accept() new connections                                  ║  │                 │
       │                  │                   │                │ ║ - recv() messages with length-prefix framing                ║  │                 │
       │                  │                   │                │ ║ - deserialize and push to event_queue_                      ║  │                 │
       │                  │                   │                │ ║ - send() queued responses                                   ║  │                 │
       │                  │                   │                │ ╚══════════════════════════════════════════════════════════════╝  │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │<─────────────────────────────────────────────────┤                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [34] LOG: "EpollWorker started - listening for connections"       │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │ PHASE 3: Start WorkerPool (lines 88-89)
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [35] Start WorkerPool             │                │               │                 │                 │                 │
       │                  ├─────────────────────────────────────────────────────────────────>│               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │ [36] Spawn worker threads       │                 │                 │
       │                  │                   │                │                │ running_ = true                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │ [37] for i in 0..4:             │                 │                 │
       │                  │                   │                │                │   workers_[i] = std::thread(&WorkerPool::worker_loop, this, i)
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │ ╔══════════════════════════════════════════════════════════════╗
       │                  │                   │                │                │ ║ WORKER THREADS RUNNING IN BACKGROUND (4 threads)            ║
       │                  │                   │                │                │ ║ Each worker:                                                ║
       │                  │                   │                │                │ ║ - event_queue_.pop() (blocking)                             ║
       │                  │                   │                │                │ ║ - process_event(event)                                      ║
       │                  │                   │                │                │ ║ - switch on message type:                                   ║
       │                  │                   │                │                │ ║   - CLIENT_REQUEST -> handle_client_request()              ║
       │                  │                   │                │                │ ║   - NODE_REGISTRATION -> handle_node_registration()        ║
       │                  │                   │                │                │ ║   - PREFILL_COMPLETE -> handle_prefill_complete()           ║
       │                  │                   │                │                │ ║   - DECODE_COMPLETE -> handle_decode_complete()             ║
       │                  │                   │                │                │ ║   - HEARTBEAT -> handle_heartbeat()                         ║
       │                  │                   │                │                │ ║   - RDMA_READY -> handle_rdma_ready()                       ║
       │                  │                   │                │                │ ╚══════════════════════════════════════════════════════════════╝
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │<─────────────────────────────────────────────────────────────────┤               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [38] LOG: "WorkerPool started with 4 workers"      │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │ PHASE 4: State Transition (lines 95-103)
       │                  │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [39] Set state = WAITING_FOR_NODES│                │               │                 │                 │                 │
       │                  │ running_ = true   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ [40] LOG: "Orchestrator ready - waiting for node registrations"   │                 │                 │                 │
       │                  │ LOG: "Expecting: 2 prefill nodes, 3 decode nodes"│                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │<─────────────────┤                   │                │                │               │                 │                 │                 │
       │ SUCCESS          │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │ [41] Main thread enters running loop│                │                │               │                 │                 │                 │
       │ while (orchestrator_engine.is_running()) {            │                │               │                 │                 │                 │
       │   sleep(1s);     │                   │                │                │               │                 │                 │                 │
       │ }                │                   │                │                │               │                 │                 │                 │
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │ NOW WAITING FOR NODE REGISTRATIONS - Orchestrator State: WAITING_FOR_NODES
       │                  │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
       │                  │                   │                │                │               │                 │                 │                 │
       │                  │                   │    ┌─────────────────────────────────────────────────────────────────────────────────────────┐
       │                  │                   │    │ I/O Thread epoll_wait() will detect:                                                     │
       │                  │                   │    │ - New connections from prefill/decode nodes (fd=11 readable -> accept())                │
       │                  │                   │    │ - NODE_REGISTRATION messages from nodes                                                 │
       │                  │                   │    │ - HEARTBEAT messages from nodes                                                          │
       │                  │                   │    │                                                                                          │
       │                  │                   │    │ Workers will process NODE_REGISTRATION:                                                 │
       │                  │                   │    │ 1. Add to NodeRegistry                                                                   │
       │                  │                   │    │ 2. Add to RdmaExchangeTracker                                                            │
       │                  │                   │    │ 3. When all nodes registered -> build QP maps                                            │
       │                  │                   │    │ 4. Transition to State::QP_EXCHANGING                                                    │
       │                  │                   │    │ 5. Broadcast BROADCAST_MEMBER_INFO to all nodes                                          │
       │                  │                   │    │ 6. Transition to State::WAITING_FOR_READY                                                │
       │                  │                   │    │ 7. When all RDMA_READY received -> State::RUNNING                                        │
       │                  │                   │    │ 8. Start accepting CLIENT_REQUEST messages                                               │
       │                  │                   │    └─────────────────────────────────────────────────────────────────────────────────────────┘
       │                  │                   │                │                │               │                 │                 │                 │

══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
ORCHESTRATOR STARTUP COMPLETE - READY TO ACCEPT NODE REGISTRATIONS
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

Key Points:
1. Orchestrator is ready BEFORE any nodes connect
2. State machine: STARTING -> WAITING_FOR_NODES -> QP_EXCHANGING -> WAITING_FOR_READY -> RUNNING
3. Two separate TCP servers: client (port 8080) for client requests, server (port 9090) for node connections
4. I/O thread handles ALL network I/O, workers process events from queue
5. RdmaExchangeTracker coordinates node registration and readiness synchronization
```

## 2. Prefill/Decode Node Startup Sequence

```
prefill_node_main.cpp (or decode_node_main.cpp) → Node Initialization & Startup

┌─────────┐  ┌────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│prefill_ │  │Node        │  │RdmaTransport │  │RdmaStorage   │  │TCPServer    │  │EpollWorker  │  │WorkerPool    │  │Membership    │  │Membership    │
│main()   │  │            │  │              │  │(KV cache)    │  │(server TCP) │  │(I/O thread) │  │(N workers)   │  │Manager       │  │Handler       │
│         │  │            │  │              │  │              │  │             │  │             │  │              │  │(hash ring)   │  │(RDMA setup)  │
└────┬────┘  └─────┬──────┘  └──────┬───────┘  └──────┬───────┘  └──────┬──────┘  └──────┬──────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
     │             │                 │                 │                 │                │                │                 │                 │
     │ [1] Parse CLI args            │                 │                 │                │                │                 │                 │
     │ --config prefill_node.yaml    │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │ [2] Load config               │                 │                 │                │                │                 │                 │
     │ - node_id: "prefill-01"       │                 │                 │                │                │                 │                 │
     │ - role: PREFILL               │                 │                 │                │                │                 │                 │
     │ - server_socket_port: 9091    │                 │                 │                │                │                 │                 │
     │ - monitoring_host: "orchestrator.local"         │                 │                │                │                 │                 │
     │ - monitoring_port: 9090       │                 │                 │                │                │                 │                 │
     │ - rdma_device: "mlx5_0"       │                 │                 │                │                │                 │                 │
     │ - peers: [prefill-02, decode-01, decode-02, decode-03]           │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │ [3] new Node(config)          │                 │                 │                │                │                 │                 │
     ├────────────>│                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │ CONSTRUCTOR PHASE - Initialize all components (prefill_node.cpp:14-40)
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [4] Create RdmaStorage            │                 │                │                │                 │                 │
     │             │ (config.memory_pool_size)         │                 │                │                │                 │                 │
     │             ├─────────────────────────────────>│                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │ [5] Allocate KV cache memory    │                │                 │                 │
     │             │                 │                 │ - malloc or GPU memory           │                │                 │                 │
     │             │                 │                 │ - size: config.memory_pool_size  │                │                 │                 │
     │             │<─────────────────────────────────┤                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [6] Create RdmaTransport          │                 │                │                │                 │                 │
     │             │ (config.rdma_device, config.gid_index)              │                │                │                 │                 │
     │             ├────────────────>│                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [7] Initialize (deferred)          │                │                │                 │                 │
     │             │                 │ - device_list_, context_, pd_ all null            │                │                 │                 │
     │             │                 │ - Will be initialized in start()  │                │                │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [8] Create TCPServer (server_tcp_server_)           │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────>│                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │ [9] Initialize │                │                 │                 │
     │             │                 │                 │                 │ - socket_fd_ = -1              │                 │                 │
     │             │<─────────────────────────────────────────────────┤                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [10] Create EventQueues           │                 │                │                │                 │                 │
     │             │ - client_event_queue_             │                 │                │                │                 │                 │
     │             │ - monitoring_event_queue_         │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [11] Create MembershipManager     │                 │                │                │                 │                 │
     │             ├───────────────────────────────────────────────────────────────────────────────────────────────────────>│                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │ [12] Initialize│
     │             │                 │                 │                 │                │                │                 │ - hash_ring_: ConsistentHashRing
     │             │                 │                 │                 │                │                │                 │ - nodes_: unordered_map<node_id, NodeInfo>
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [13] Create EpollWorker           │                 │                │                │                 │                 │
     │             │ (monitoring_event_queue_, server_tcp_server_)       │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────────────────────>│                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [14] Initialize│                 │                 │
     │             │                 │                 │                 │                │ - epoll_fd_ = epoll_create1()   │                 │
     │             │                 │                 │                 │                │ - event_queue_ref               │                 │
     │             │                 │                 │                 │                │ - server_tcp_server_ref         │                 │
     │             │<─────────────────────────────────────────────────────────────────┤                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [15] Create WorkerPool            │                 │                │                │                 │                 │
     │             │ (config.worker_pool_size, monitoring_event_queue_, epoll_worker_)│                │                 │                 │
     │             ├─────────────────────────────────────────────────────────────────────────────────>│                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │ [16] Initialize│                 │
     │             │                 │                 │                 │                │                │ - num_workers_: config.worker_pool_size
     │             │                 │                 │                 │                │                │ - running_: false                │
     │             │<─────────────────────────────────────────────────────────────────────────────────┤                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [17] Create MembershipHandler     │                 │                │                │                 │                 │
     │             ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────>│
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │ [18] Initialize
     │             │                 │                 │                 │                │                │                 │                 │ - rdma_transport_ref
     │             │                 │                 │                 │                │                │                 │                 │ - membership_manager_ref
     │             │<───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [19] Constructor complete         │                 │                │                │                 │                 │
     │<────────────┤                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │ [20] Call server_node.start() │                 │                 │                │                │                 │                 │
     ├────────────>│                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │ START PHASE - Initialize RDMA, connect to orchestrator, establish membership (prefill_node.cpp:42-150+)
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [21] LOG: "Starting prefill node: prefill-01"       │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 1: RDMA Infrastructure Initialization (lines 44-74)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [22] Initialize RDMA transport    │                 │                │                │                 │                 │
     │             ├────────────────>│                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [23] Get RDMA device list         │                │                │                 │                 │
     │             │                 │ device_list_ = ibv_get_device_list()              │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [24] Open device "mlx5_0"         │                │                │                 │                 │
     │             │                 │ context_ = ibv_open_device(device_list_[0])       │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [25] Query port attributes        │                │                │                 │                 │
     │             │                 │ ibv_query_port(context_, 1, &port_attr)           │                │                 │                 │
     │             │                 │ Verify: port_attr.state == IBV_PORT_ACTIVE        │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [26] Create Protection Domain     │                │                │                 │                 │
     │             │                 │ pd_ = ibv_alloc_pd(context_)      │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [27] Create Completion Queue      │                │                │                 │                 │
     │             │                 │ cq_ = ibv_create_cq(context_, CQ_SIZE, ...)       │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │ SUCCESS         │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [28] LOG: "RDMA transport initialized on device mlx5_0"             │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [29] Register memory with RDMA    │                 │                │                │                 │                 │
     │             ├────────────────>│                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [30] Get memory pointer and size  │                │                │                 │                 │
     │             │                 ├────────────────>│                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │ returns: ptr, size              │                │                 │                 │
     │             │                 │<────────────────┤                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [31] Register memory region       │                │                │                 │                 │
     │             │                 │ mr_ = ibv_reg_mr(pd_, ptr, size,  │                │                │                 │                 │
     │             │                 │     IBV_ACCESS_LOCAL_WRITE |      │                │                │                 │                 │
     │             │                 │     IBV_ACCESS_REMOTE_READ |      │                │                │                 │                 │
     │             │                 │     IBV_ACCESS_REMOTE_WRITE)      │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ Store: mem_addr_ = ptr, mem_size_ = size, lkey_, rkey_            │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [32] LOG: "Registered RDMA memory: size={size} bytes"               │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [33] Start RDMA completion poller │                 │                │                │                 │                 │
     │             ├────────────────>│                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [34] Spawn completion poller thread               │                │                 │                 │
     │             │                 │ completion_thread_ = std::thread(&RdmaTransport::poll_completions, this)            │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ ╔══════════════════════════════════════════════════════════════╗  │                 │                 │
     │             │                 │ ║ RDMA COMPLETION THREAD RUNNING IN BACKGROUND                ║  │                 │                 │
     │             │                 │ ║ - ibv_poll_cq(cq_, completions)                             ║  │                 │                 │
     │             │                 │ ║ - Process RDMA READ/WRITE completions                       ║  │                 │                 │
     │             │                 │ ║ - Notify application of transfer completion                 ║  │                 │                 │
     │             │                 │ ╚══════════════════════════════════════════════════════════════╝  │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [35] Pre-create Queue Pairs for all peers           │                │                │                 │                 │
     │             │ peer_ids = [prefill-02, decode-01, decode-02, decode-03]            │                │                 │                 │
     │             ├────────────────>│                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │ [36] for each peer_id in peer_ids:│                │                │                 │                 │
     │             │                 │   Create QP:    │                 │                │                │                 │                 │
     │             │                 │   qp_init_attr.qp_type = IBV_QPT_RC               │                │                 │                 │
     │             │                 │   qp = ibv_create_qp(pd_, &qp_init_attr)          │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │   Modify QP to INIT state:        │                │                │                 │                 │
     │             │                 │   ibv_modify_qp(qp, IBV_QPS_INIT) │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │   Store in qp_map_[peer_id] = {qp, qp_num, ...}   │                │                 │                 │
     │             │                 │                 │                 │                │                │                │                 │
     │             │                 │ [37] Created 4 queue pairs (1 per peer)           │                │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [38] LOG: "Pre-created 4 queue pairs for peer nodes"                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 2: Network Setup (lines 76-99)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [39] Bind server TCP server       │                 │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────>│                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │ [40] socket() -> fd=20          │                 │                 │
     │             │                 │                 │                 │ bind(0.0.0.0:9091)              │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [41] server_tcp_server_.listen()  │                 │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────>│                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │ [42] listen(fd=20, backlog=128) │                 │                 │
     │             │                 │                 │                 │ set_nonblocking(fd=20)          │                 │                 │
     │             │<─────────────────────────────────────────────────┤                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [43] LOG: "Server TCP listening on port 9091"      │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [44] Start EpollWorker            │                 │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────────────────────>│                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [45] Add server listen socket to epoll
     │             │                 │                 │                 │                │ epoll_ctl(ADD, fd=20, EPOLLIN | EPOLLET)
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [46] Spawn I/O thread                              │
     │             │                 │                 │                 │                │ io_thread_ = std::thread(&EpollWorker::io_loop, this)
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ ╔══════════════════════════════════════════════════════════════╗
     │             │                 │                 │                 │                │ ║ I/O THREAD RUNNING IN BACKGROUND                            ║
     │             │                 │                 │                 │                │ ║ - epoll_wait() on fd=20, orchestrator connection            ║
     │             │                 │                 │                 │                │ ║ - recv() messages with length-prefix framing                ║
     │             │                 │                 │                 │                │ ║ - push events to monitoring_event_queue_                    ║
     │             │                 │                 │                 │                │ ╚══════════════════════════════════════════════════════════════╝
     │             │<─────────────────────────────────────────────────────────────────┤                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [47] LOG: "EpollWorker started"   │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 3: Membership Initialization (lines 101-115)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [48] Build initial node list      │                 │                │                │                 │                 │
     │             │ initial_nodes = [self: prefill-01, peers: prefill-02, decode-01, decode-02, decode-03]                │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [49] Initialize membership        │                 │                │                │                 │                 │
     │             ├───────────────────────────────────────────────────────────────────────────────────────────────────────>│                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │ [50] Add nodes to hash ring
     │             │                 │                 │                 │                │                │                 │ for each node in initial_nodes:
     │             │                 │                 │                 │                │                │                 │   hash_ring_.add_node(node)
     │             │                 │                 │                 │                │                │                 │   nodes_[node_id] = NodeInfo
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │ NOTE: RDMA connections NOT created yet
     │             │                 │                 │                 │                │                │                 │ (deferred until after heartbeat/registration)
     │             │<───────────────────────────────────────────────────────────────────────────────────────────────────────┤                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [51] LOG: "Membership ring initialized with 5 nodes"                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 4: Orchestrator Connection (lines 117-126)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [52] Connect to orchestrator      │                 │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────────────────────>│                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [53] socket() -> fd=21         │                 │
     │             │                 │                 │                 │                │ set_nonblocking(fd=21)         │                 │
     │             │                 │                 │                 │                │ connect(orchestrator.local:9090)                │
     │             │                 │                 │                 │                │ (non-blocking connect)         │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [54] Add to epoll              │                 │
     │             │                 │                 │                 │                │ epoll_ctl(ADD, fd=21, EPOLLOUT | EPOLLIN | EPOLLET)
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [55] Wait for connection to complete (EPOLLOUT) │
     │             │                 │                 │                 │                │ When connected, send initial NODE_REGISTRATION:  │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [56] Build NODE_REGISTRATION message            │
     │             │                 │                 │                 │                │ {                              │                 │
     │             │                 │                 │                 │                │   type: NODE_REGISTRATION,     │                 │
     │             │                 │                 │                 │                │   node_id: "prefill-01",       │                 │
     │             │                 │                 │                 │                │   role: PREFILL,               │                 │
     │             │                 │                 │                 │                │   server_port: 9091,           │                 │
     │             │                 │                 │                 │                │   rdma_qp_info: {              │                 │
     │             │                 │                 │                 │                │     qp_nums: [qp1, qp2, ...],  │                 │
     │             │                 │                 │                 │                │     gid: {...},                │                 │
     │             │                 │                 │                 │                │     lid: ...,                  │                 │
     │             │                 │                 │                 │                │   }                            │                 │
     │             │                 │                 │                 │                │ }                              │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │ [57] send() NODE_REGISTRATION to orchestrator   │
     │             │                 │                 │                 │                │ (length-prefix framing)        │                 │
     │             │<─────────────────────────────────────────────────────────────────┤                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [58] LOG: "Connected to orchestrator and sent NODE_REGISTRATION"  │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 5: Membership Handler Start (lines 128-130)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [59] Start MembershipHandler      │                 │                │                │                 │                 │
     │             ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────>│
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │ [60] Initialize
     │             │                 │                 │                 │                │                │                 │                 │ running_ = true
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │ ╔══════════════════════════════════════════════════════════════╗
     │             │                 │                 │                 │                │                │                 │                 │ ║ MEMBERSHIP HANDLER RESPONSIBILITIES:                         ║
     │             │                 │                 │                 │                │                │                 │                 │ ║ 1. Listen for BROADCAST_MEMBER_INFO from orchestrator        ║
     │             │                 │                 │                 │                │                │                 │                 │ ║    (contains QP maps for all nodes)                          ║
     │             │                 │                 │                 │                │                │                 │                 │ ║ 2. Extract QP info for each peer node                        ║
     │             │                 │                 │                 │                │                │                 │                 │ ║ 3. Call rdma_transport_.connect_qp(peer_id, peer_qp_info)   ║
     │             │                 │                 │                 │                │                │                 │                 │ ║    for each peer                                             ║
     │             │                 │                 │                 │                │                │                 │                 │ ║ 4. Modify QPs to RTR and RTS states                          ║
     │             │                 │                 │                 │                │                │                 │                 │ ║ 5. Send RDMA_READY to orchestrator when all QPs connected    ║
     │             │                 │                 │                 │                │                │                 │                 │ ╚══════════════════════════════════════════════════════════════╝
     │             │<───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [61] LOG: "MembershipHandler started - waiting for BROADCAST_MEMBER_INFO"           │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │ PHASE 6: Worker Pool Start (lines 133-135)
     │             │ ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [62] Start WorkerPool             │                 │                │                │                 │                 │
     │             ├─────────────────────────────────────────────────────────────────────────────────────>│                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │ [63] Spawn worker threads       │
     │             │                 │                 │                 │                │                │ for i in 0..num_workers:        │
     │             │                 │                 │                 │                │                │   workers_[i] = std::thread(...)│
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │ ╔══════════════════════════════════════════════════════════════╗
     │             │                 │                 │                 │                │                │ ║ WORKER THREADS RUNNING IN BACKGROUND                         ║
     │             │                 │                 │                 │                │                │ ║ Each worker:                                                ║
     │             │                 │                 │                 │                │                │ ║ - monitoring_event_queue_.pop() (blocking)                  ║
     │             │                 │                 │                 │                │                │ ║ - process_event(event)                                      ║
     │             │                 │                 │                 │                │                │ ║ - switch on message type:                                   ║
     │             │                 │                 │                 │                │                │ ║   - BROADCAST_MEMBER_INFO -> MembershipHandler              ║
     │             │                 │                 │                 │                │                │ ║   - ASSIGN_REQUEST -> handle_assign_request()               ║
     │             │                 │                 │                 │                │                │ ║   - TRANSFER_READY -> handle_transfer_ready()               ║
     │             │                 │                 │                 │                │                │ ╚══════════════════════════════════════════════════════════════╝
     │             │<─────────────────────────────────────────────────────────────────────────────────────┤                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ [64] LOG: "WorkerPool started"    │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │<────────────────┤                 │                 │                │                │                 │                 │
     │             │ SUCCESS         │                 │                 │                │                │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │ [65] Main thread enters running loop            │                 │                │                │                 │                 │
     │ while (server_node.is_running()) {              │                 │                │                │                 │                 │
     │   sleep(1s); │                 │                 │                │                │                 │                 │                 │
     │ }            │                 │                 │                │                │                 │                 │                 │
     │             │                 │                 │                 │                │                │                 │                 │
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │ NODE STARTUP COMPLETE - WAITING FOR ORCHESTRATOR TO SEND BROADCAST_MEMBER_INFO
     │             │ ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
     │             │                 │                 │                 │                │                │                 │                 │
     │             │                 │    ┌─────────────────────────────────────────────────────────────────────────────────────────┐
     │             │                 │    │ WHAT HAPPENS NEXT (asynchronously):                                                      │
     │             │                 │    │                                                                                          │
     │             │                 │    │ 1. Orchestrator receives NODE_REGISTRATION from this node and all other nodes          │
     │             │                 │    │ 2. When all expected nodes registered (2 prefill + 3 decode), orchestrator:            │
     │             │                 │    │    - Builds complete QP map (all nodes' QP info)                                       │
     │             │                 │    │    - Sends BROADCAST_MEMBER_INFO to all nodes                                           │
     │             │                 │    │                                                                                          │
     │             │                 │    │ 3. I/O thread receives BROADCAST_MEMBER_INFO on fd=21                                   │
     │             │                 │    │ 4. Event pushed to monitoring_event_queue_                                              │
     │             │                 │    │ 5. Worker pops event and routes to MembershipHandler                                   │
     │             │                 │    │                                                                                          │
     │             │                 │    │ 6. MembershipHandler processes BROADCAST_MEMBER_INFO:                                   │
     │             │                 │    │    For each peer node:                                                                  │
     │             │                 │    │      - Extract peer_qp_info                                                             │
     │             │                 │    │      - Call rdma_transport_.connect_qp(peer_id, peer_qp_info)                          │
     │             │                 │    │        * Modify local QP to RTR (Ready To Receive):                                    │
     │             │                 │    │          ibv_modify_qp(qp, IBV_QPS_RTR, remote_qp_num, remote_gid, remote_lid)         │
     │             │                 │    │        * Modify local QP to RTS (Ready To Send):                                       │
     │             │                 │    │          ibv_modify_qp(qp, IBV_QPS_RTS)                                                 │
     │             │                 │    │      - QP is now ACTIVE and ready for RDMA operations                                  │
     │             │                 │    │                                                                                          │
     │             │                 │    │ 7. After all QPs connected:                                                             │
     │             │                 │    │    - LOG: "All RDMA connections established"                                            │
     │             │                 │    │    - Send RDMA_READY message to orchestrator                                            │
     │             │                 │    │                                                                                          │
     │             │                 │    │ 8. Orchestrator waits for RDMA_READY from all nodes                                     │
     │             │                 │    │ 9. When all nodes ready, orchestrator transitions to State::RUNNING                    │
     │             │                 │    │ 10. Cluster is now operational and ready for client requests                           │
     │             │                 │    └─────────────────────────────────────────────────────────────────────────────────────────┘
     │             │                 │                 │                 │                │                │                 │                 │

══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
PREFILL/DECODE NODE STARTUP COMPLETE - REGISTERED WITH ORCHESTRATOR
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

Key Points:
1. RDMA infrastructure initialized BEFORE network connections
2. Queue pairs pre-created for all peers to avoid race conditions
3. MembershipHandler waits for orchestrator to broadcast QP maps
4. QP connections established after receiving BROADCAST_MEMBER_INFO
5. Node signals RDMA_READY to orchestrator when all QPs connected
6. Decode nodes follow same startup flow, just with NodeRole::DECODE
```

## 3. RDMA Connection Establishment Flow (Detail)

This shows what happens when the orchestrator broadcasts QP maps to nodes:

```
Orchestrator RDMA Coordination - After All Nodes Register

┌──────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│Orchestrator  │  │prefill-01   │  │prefill-02   │  │decode-01    │  │decode-02    │  │decode-03    │
│              │  │             │  │             │  │             │  │             │  │             │
└──────┬───────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
       │                 │                 │                 │                 │                 │
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │ STATE: WAITING_FOR_NODES
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │                 │                 │                 │                 │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ┤ NODE_REGISTRATION (prefill-01, QP info)            │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ NODE_REGISTRATION (prefill-02, QP info)            │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ NODE_REGISTRATION (decode-01, QP info)
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ NODE_REGISTRATION (decode-02, QP info)
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ NODE_REGISTRATION (decode-03, QP info)
       │                 │                 │                 │                 │                 │
       │ [Worker processes last NODE_REGISTRATION]          │                 │                 │
       │ RdmaExchangeTracker::register_node(decode-03)      │                 │                 │
       │                 │                 │                 │                 │                 │
       │ [Check: all_nodes_registered() == true]            │                 │                 │
       │ registered: 2 prefill + 3 decode                   │                 │                 │
       │ expected: 2 prefill + 3 decode ✓                   │                 │                 │
       │                 │                 │                 │                 │                 │
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │ TRANSITION TO: QP_EXCHANGING
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │                 │                 │                 │                 │                 │
       │ [Build complete QP map]           │                 │                 │                 │
       │ qp_map = {      │                 │                 │                 │                 │
       │   prefill-01: {qp_num, gid, lid, ...},             │                 │                 │
       │   prefill-02: {qp_num, gid, lid, ...},             │                 │                 │
       │   decode-01: {...},               │                 │                 │                 │
       │   decode-02: {...},               │                 │                 │                 │
       │   decode-03: {...}                │                 │                 │                 │
       │ }               │                 │                 │                 │                 │
       │                 │                 │                 │                 │                 │
       │ [Broadcast BROADCAST_MEMBER_INFO to all nodes]     │                 │                 │
       │                 │                 │                 │                 │                 │
       ├─ ─ ─ ─ ─ ─ ─ ─>│ BROADCAST_MEMBER_INFO(qp_map)     │                 │                 │
       ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─>│ BROADCAST_MEMBER_INFO(qp_map)     │                 │
       ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─>│ BROADCAST_MEMBER_INFO(qp_map)     │
       ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─>│ BROADCAST_MEMBER_INFO(qp_map)
       ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─>│ BROADCAST_MEMBER_INFO(qp_map)
       │                 │                 │                 │                 │                 │
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │ TRANSITION TO: WAITING_FOR_READY
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │                 │                 │                 │                 │                 │
       │                 │ [Worker receives BROADCAST_MEMBER_INFO]            │                 │
       │                 │ [MembershipHandler.process_member_info(qp_map)]    │                 │
       │                 │                 │                 │                 │                 │
       │                 │ For each peer (prefill-02, decode-01, decode-02, decode-03):        │
       │                 │   connect_qp(peer_id, peer_qp_info)                │                 │
       │                 │   - Modify QP to RTR (Ready To Receive)            │                 │
       │                 │   - Modify QP to RTS (Ready To Send)               │                 │
       │                 │                 │                 │                 │                 │
       │                 │ [4 RDMA connections established]  │                 │                 │
       │                 │ LOG: "All RDMA connections ready" │                 │                 │
       │                 │                 │                 │                 │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ┤ RDMA_READY      │                 │                 │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ RDMA_READY      │                 │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ RDMA_READY      │                 │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ RDMA_READY      │
       │<─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤ RDMA_READY
       │                 │                 │                 │                 │                 │
       │ [Worker processes last RDMA_READY]│                 │                 │                 │
       │ RdmaExchangeTracker::mark_node_ready(decode-03)    │                 │                 │
       │                 │                 │                 │                 │                 │
       │ [Check: all_nodes_ready() == true]│                 │                 │                 │
       │ ready: 5/5 nodes ✓                │                 │                 │                 │
       │                 │                 │                 │                 │                 │
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │ TRANSITION TO: RUNNING
       │ ═══════════════════════════════════════════════════════════════════════════════════════════
       │                 │                 │                 │                 │                 │
       │ LOG: "All nodes RDMA ready - cluster operational"  │                 │                 │
       │ LOG: "Now accepting client requests"               │                 │                 │
       │                 │                 │                 │                 │                 │
       │                 ┌───────────────────────────────────────────────────────────────────────┐
       │                 │ RDMA MESH TOPOLOGY NOW FULLY CONNECTED                                 │
       │                 │                                                                        │
       │                 │ prefill-01 ←───RDMA───→ prefill-02                                     │
       │                 │     ↓  ↘ ↘                  ↓ ↘ ↘                                      │
       │                 │     ↓    ↘   ↘              ↓   ↘  ↘                                   │
       │                 │  decode-01  decode-02  decode-03                                       │
       │                 │     ↑ ↗ ↗                  ↑ ↗ ↗                                       │
       │                 │     ↑ ↗   ↗                ↑ ↗   ↗                                     │
       │                 │ prefill-02 ←───RDMA───→ prefill-01                                     │
       │                 │                                                                        │
       │                 │ All nodes can now perform RDMA READ/WRITE to any peer's KV cache      │
       │                 └───────────────────────────────────────────────────────────────────────┘
       │                 │                 │                 │                 │                 │

══════════════════════════════════════════════════════════════════════════════════════════════════════════
CLUSTER OPERATIONAL - ORCHESTRATOR READY TO ACCEPT CLIENT REQUESTS
══════════════════════════════════════════════════════════════════════════════════════════════════════════
```

## Summary

### Orchestrator Startup Phases:
1. **Constructor** - Create all components (EventQueue, NodeRegistry, RequestRouter, etc.)
2. **TCP Binding** - Bind and listen on client port (8080) and server port (9090)
3. **Start I/O Thread** - EpollWorker starts epoll loop
4. **Start Workers** - WorkerPool spawns worker threads
5. **State: WAITING_FOR_NODES** - Ready to accept node registrations

### Prefill/Decode Node Startup Phases:
1. **Constructor** - Create all components (RdmaStorage, RdmaTransport, MembershipManager, etc.)
2. **RDMA Initialization** - Initialize IB device, register memory, create CQ, pre-create QPs
3. **Network Setup** - Bind and listen on server port, start EpollWorker
4. **Membership Init** - Initialize consistent hash ring with all peers
5. **Connect to Orchestrator** - Establish TCP connection, send NODE_REGISTRATION
6. **Start Handlers** - MembershipHandler and WorkerPool start
7. **Wait for QP Exchange** - Receive BROADCAST_MEMBER_INFO, connect all QPs
8. **Signal Ready** - Send RDMA_READY to orchestrator

### Synchronization Points:
- Orchestrator waits for all expected node registrations
- Orchestrator broadcasts QP maps to all nodes
- Nodes establish RDMA connections
- Nodes signal RDMA_READY
- Orchestrator transitions to RUNNING state
- Cluster operational

This architecture ensures proper ordering of initialization and RDMA connection establishment before accepting client requests.
