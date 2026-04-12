```mermaid
flowchart TD
    A[Start RTSPServer.cpp] --> B[Create TaskScheduler and UsageEnvironment]
    B --> C[VideoInput::createNew]
    C --> D{fHaveInitialized?}
    D -->|No| E[Call initialize]
    D -->|Yes| F[Create VideoInput Instance]
    
    E --> G[imp_init]
    G --> H[ImpSystemInit]
    H --> I[IMP_ISP_Open]
    I --> J[IMP_ISP_AddSensor]
    J --> K[IMP_ISP_EnableSensor]
    K --> L[IMP_System_Init]
    L --> M[IMP_ISP_EnableTuning]
    M --> N[framesource_init]
    
    N --> O[IMP_FrameSource_CreateChn]
    O --> P[IMP_FrameSource_SetChnAttr]
    P --> Q[encoder_init]
    Q --> R[IMP_Encoder_CreateGroup]
    R --> S[IMP_Encoder_CreateChn]
    S --> T[IMP_Encoder_RegisterChn]
    T --> U[IMP_System_Bind FrameSource→Encoder]
    U --> V[IMP_FrameSource_SetFrameDepth]
    V --> W[IMP_FrameSource_EnableChn]
    W --> X[sample_audio_amic_init]
    X --> F
    
    F --> Y[Create RTSPServer]
    Y --> Z[Create ServerMediaSession main]
    Z --> AA[Add H264VideoServerMediaSubsession]
    AA --> BB[Add G711AudioStreamServerMediaSubsession]
    BB --> CC[rtspServer->addServerMediaSession]
    CC --> DD[env->taskScheduler().doEventLoop]
    
    AA --> EE[H264VideoServerMediaSubsession::createNew]
    EE --> FF[Creates H264VideoStreamSource]
    FF --> GG[H264VideoStreamSource Constructor]
    GG --> HH[Create EventTrigger]
    HH --> II[pthread_create PollingThread]
    II --> JJ[fInput.streamOn]
    
    JJ --> KK[VideoInput::streamOn]
    KK --> LL[IMP_Encoder_RequestIDR]
    LL --> MM[IMP_Encoder_StartRecvPic]
    
    II --> NN[PollingThread Loop]
    NN --> OO[sem_wait]
    OO --> PP[fInput.pollingStream]
    PP --> QQ[IMP_Encoder_PollingStream 2000ms timeout]
    QQ --> RR{Frame Available?}
    RR -->|Yes| SS[triggerEvent]
    RR -->|No| TT[Continue Loop]
    SS --> UU[incomingDataHandler1]
    UU --> VV[fInput.getStream]
    VV --> WW[IMP_Encoder_GetStream]
    WW --> XX[Copy Frame Data]
    XX --> YY[IMP_Encoder_ReleaseStream]
    YY --> ZZ[afterGetting - Send to Live555]
    TT --> OO
    ZZ --> AAA[Live555 RTP Transmission]
    
    DD --> BBB[RTSP Client Connects]
    BBB --> CCC[Live555 Handles RTSP Protocol]
    CCC --> DDD[Client Requests Stream]
    DDD --> EEE[doGetNextFrame Called]
    EEE --> FFF[sem_post to Wake PollingThread]
    FFF --> OO
    
    style MM fill:#99ff99
    style W fill:#99ff99
    style QQ fill:#ccffcc
    style WW fill:#ccffcc
    style II fill:#ffcc99
    style JJ fill:#ffcc99
```