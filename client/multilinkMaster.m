%% initialize
clear
close all
Logger = util.LoggerClass;

nNode = 4;

MasterConfig = util.readjsonfile(fullfile("config","master.json"));

for iNode = 1:nNode
    Node(iNode).addr = MasterConfig.SLAVE_TCP_ADDR{iNode};
    Node(iNode).tcpPort = MasterConfig.SLAVE_TCP_PORT(iNode);
    Node(iNode).tcpSock = tcpclient(Node(iNode).addr, Node(iNode).tcpPort);
    Node(iNode).udpSock = udpport("datagram","IPV4","LocalPort", ...
        MasterConfig.SLAVE_UDP_PORT(iNode));
    Logger.info("Connected to Node %d.",iNode)
end
Logger.info("initialized.")

global isStartButtonPushed %#ok<*GVMIS>
global isExitButtonPushed
global isSkipButtonPushed
global isReady

isStartButtonPushed = false;
isExitButtonPushed = false;
isReady = false;

s = util.readcplxfile("data\multitone.dat");
sf = fft(s);
caliKitData = load("data\Hc_4_8GHz.mat");
hcB2b = caliKitData.Hc;
for iLink = 1:6
    b2bData(iLink) = load(fullfile("recieve_data","b2b", ...
        MasterConfig.B2B_DATA_DIR,string(iLink))); %#ok<*SAGROW> 
    b2bData(iLink).ctf = b2bData(iLink).ctf./hcB2b;
end

role = [1 2 3;     % Tx
        2 3 4;     % Rx1
        3 4 0;     % Rx2
        4 0 0;];   % Rx3

%% 送受信開始
Logger.info("stream start.")

screenSize = get(0,"screensize");
screenWidth = screenSize(3);
screenHeight = screenSize(4);
uiWidth = ceil(screenWidth*2/3);
uiHeight = ceil(screenHeight*2/3);
uiPosition = [screenWidth/2-uiWidth/2 screenHeight/2-uiHeight/2  uiWidth uiHeight];

fig = uifigure("Position",uiPosition);
g = uigridlayout(fig,[3 6]);
g.RowHeight = {'1x','1x','fit'};
g.ColumnWidth = {'1x','1x','1x','1x','1x','1x'};

for iPanel = 1:6
    mapping = [[5 6]; [1 2];[3 4]];
    panel(iPanel) = uipanel(g);
    panel(iPanel).Layout.Row = ceil(iPanel/3);
    panel(iPanel).Layout.Column = mapping(rem(iPanel,3)+1,:);
end
b = uibutton(g, ...
    "Text","Start", ...
    "ButtonPushedFcn", @(src,event) startButtonPushed);
b.Layout.Row = 3;
b.Layout.Column = 2;
b = uibutton(g, ...
    "Text","Exit", ...
    "ButtonPushedFcn", @(src,event) exitButtonPushed);
b.Layout.Row = 3;
b.Layout.Column = 3;
ef = uieditfield(g);
ef.Layout.Row = 3;
ef.Layout.Column = 4;
cb = uicheckbox(g,"Text","send UDP");
cb.Layout.Row = 3;
cb.Layout.Column = 5;

while(true)
    isReady = true;
    isSkipButtonPushed = false;
    while(~isStartButtonPushed && ~isExitButtonPushed)
        pause(.1)
    end
    if(isExitButtonPushed)
        break
    else
        isStartButtonPushed = false;
        isReady = false;
    end
    powLink = zeros(8,8,6);
    iLink = 1;

    for iPhase = 1:size(role,2)
        txNode = role(1,iPhase);
        rxNode = role(2:4,iPhase);
        rxNode = rxNode(rxNode > 0);

        flush(Node(txNode).tcpSock)
        Logger.info("send command to start transmit")
        Node(txNode).tcpSock.write("1")  %start tx
        tcpData = waittcpdata(Node(txNode).tcpSock);
        Logger.info("TCP message received :"+tcpData)
        if(tcpData ~= '1')
            Logger.error("Unexpected Error")
        end
        pause(.2)

        for jRxNode = 1:length(rxNode)
            Logger.info("send command to start recieve")
            Node(rxNode(jRxNode)).udpSock.flush
            Node(rxNode(jRxNode)).tcpSock.write( ...
                "3$" + txNode + "$" + ef.Value + "$" + cb.Value)  %start rx
        end
        for jRxNode = 1:length(rxNode)
            tcpData = waittcpdata(Node(rxNode(jRxNode)).tcpSock);
            Logger.info("TCP message received :"+tcpData)
            if(tcpData ~= '3')
                Logger.error("Unexpected Error")
            end
        end
        if(cb.Value)
            for jRxNode = 1:length(rxNode)
                rcvData = [];
                while(numel(rcvData) ~= 256*8*8*2)
                    if(Node(rxNode(jRxNode)).udpSock.NumDatagramsAvailable)
                        udpData = Node(rxNode(jRxNode)).udpSock.read( ...
                            Node(rxNode(jRxNode)).udpSock.NumDatagramsAvailable,"double");
                        rcvData = [rcvData udpData.Data]; %#ok<*AGROW> 
                    end
                    pause(.01)
                    if(isSkipButtonPushed)
                        break %#ok<*UNRCH> 
                    end
                end
                if(~isSkipButtonPushed)
                    Logger.info("Recieved UDP Data Link "+iLink)
                    yt = reshape(util.tocplx(rcvData.'),256,8,8);
                    yf = fft(yt);
                    ctf = zeros(128,8,8);
                    for iTx = 1:8
                        ctf(:,:,iTx) = util.fixctf((yf(:,:,iTx)./sf)./b2bData(iLink).ctf(:,:,iTx));
                    end
                    powLinkTmp = pow2db(abs(ctf).^2);
                    powLink(:,:,iLink) =  squeeze(mean(powLinkTmp));
                    iLink = iLink+1;
                end
            end
        end


        Logger.info("send command to stop transmit")
        Node(txNode).tcpSock.write("2")  %end tx
        tcpData = waittcpdata(Node(txNode).tcpSock);
        Logger.info("TCP message received :"+tcpData)
        if(tcpData ~= '2')
            Logger.error("Unexpected Error")
        end
    end
    if(cb.Value)
        for iLink = 1:6
            heatmap(panel(iLink),powLink(:,:,iLink), ...
                "ColorLimits",[-70 -30], "Colormap",jet)
            panel(iLink).Title = "Link: " + iLink;
        end
    end
end

clear Node
close(fig)


function tcpData = waittcpdata(sock)
while(true)
    numBytesAvailable = sock.NumBytesAvailable;
    if(logical(numBytesAvailable))
        tcpData = sock.read(numBytesAvailable,"char");
        break
    end
    pause(.01)
end
end

function startButtonPushed()
global isStartButtonPushed
global isReady
if(isReady)
    isStartButtonPushed = true;
end
end

function exitButtonPushed()
global isExitButtonPushed
global isSkipButtonPushed
global isReady
if(isReady)
    isExitButtonPushed = true;
else
    isSkipButtonPushed = true;
end
end

