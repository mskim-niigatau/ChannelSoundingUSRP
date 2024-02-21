%% initialize
clear
close all

nNode = 4;
phase = 2;

node(1).addr = "192.168.1.201";
node(1).tcpPort = 60001;

node(2).addr = "192.168.1.202";
node(2).tcpPort = 60001;

node(3).addr = "192.168.1.203";
node(3).tcpPort = 60001;

node(4).addr = "192.168.1.204";
node(4).tcpPort = 60001;

for i = 1:nNode
    node(i).tcpSock = tcpclient(node(i).addr, node(i).tcpPort);
end
Logger = util.LoggerClass;
Logger.info("initialized.")

udpSock(1) = udpport("datagram","IPV4","LocalPort",50000);
udpSock(2) = udpport("datagram","IPV4","LocalPort",50002);
udpSock(3) = udpport("datagram","IPV4","LocalPort",50003);
udpSock(4) = udpport("datagram","IPV4","LocalPort",50004);

global isStartButtonPushed %#ok<*GVMIS>
global isExitButtonPushed
global isSkipButtonPushed
global isReady

isStartButtonPushed = false;
isExitButtonPushed = false;
isReady = false;


role = [1 2;     % Tx
    2 1;     % Rx1
    3 3;     % Rx2
    4 4;];   % Rx3

s = util.readcplxfile("data\multitone.dat");
sf = fft(s);
for iLink = 1:3
    b2bData(iLink) = load(fullfile("recieve_data","b2b","aoa", ...
        sprintf("%d-%d.mat",role(1,phase),role(iLink+1,phase))));
    % b2bData(iLink).ctf = b2bData(iLink).ctf;
end

%% 送受信開始
Logger.info("stream start.")

screenSize = get(0,"screensize");
screenWidth = screenSize(3);
screenHeight = screenSize(4);
uiWidth = ceil(screenWidth*2/3);
uiHeight = ceil(screenHeight*2/5);
uiPosition = [screenWidth/2-uiWidth/2 screenHeight/2-uiHeight/2  uiWidth uiHeight];

fig = uifigure("Position",uiPosition);
g = uigridlayout(fig,[2 6]);
g.RowHeight = {'1x','fit'};
g.ColumnWidth = {'1x','1x','1x','1x','1x','1x'};

for iPanel = 1:3
    mapping = [[5 6]; [1 2];[3 4]];
    panel(iPanel) = uipanel(g);
    panel(iPanel).Layout.Row = ceil(iPanel/3);
    panel(iPanel).Layout.Column = mapping(rem(iPanel,3)+1,:);
end
b = uibutton(g, ...
    "Text","Start", ...
    "ButtonPushedFcn", @(src,event) startButtonPushed);
b.Layout.Row = 2;
b.Layout.Column = 2;
b = uibutton(g, ...
    "Text","Exit", ...
    "ButtonPushedFcn", @(src,event) exitButtonPushed);
b.Layout.Row = 2;
b.Layout.Column = 3;
ef = uieditfield(g);
ef.Layout.Row = 2;
ef.Layout.Column = 4;
cb = uicheckbox(g,"Text","send UDP");
cb.Layout.Row = 2;
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

    txNode = role(1,phase);
    rxNode = role(2:4,phase);
    rxNode = rxNode(rxNode > 0);

    flush(node(txNode).tcpSock)
    Logger.info("send command to start transmit")
    node(txNode).tcpSock.write("1")  %start tx
    tcpData = waittcpdata(node(txNode).tcpSock);
    Logger.info("TCP message received :"+tcpData)
    if(tcpData ~= '1')
        Logger.error("Unexpected Error")
    end

    for jRxNode = 1:length(rxNode)
        Logger.info("send command to start recieve")
        udpSock(rxNode(jRxNode)).flush
        node(rxNode(jRxNode)).tcpSock.write( ...
            "3$" + txNode + "$" + ef.Value + "$" + cb.Value)  %start rx
    end
    for jRxNode = 1:length(rxNode)
        tcpData = waittcpdata(node(rxNode(jRxNode)).tcpSock);
        Logger.info("TCP message received :"+tcpData)
        if(tcpData ~= '3')
            Logger.error("Unexpected Error")
        end
    end
    if(cb.Value)
        for jRxNode = 1:length(rxNode)
            rcvData = [];
            while(numel(rcvData) ~= 256*8*8*2)
                if(udpSock(rxNode(jRxNode)).NumDatagramsAvailable)
                    udpData = udpSock(rxNode(jRxNode)).read(udpSock(rxNode(jRxNode)).NumDatagramsAvailable,"double");
                    rcvData = [rcvData udpData.Data]; %#ok<AGROW>
                end
                pause(.01)
                if(isSkipButtonPushed)
                    break
                end
            end
            if(~isSkipButtonPushed)
                Logger.info("Recieved UDP Data Link "+iLink)
                yt = reshape(util.tocplx(rcvData.'),256,8,8);
                yf = fft(yt);
                ctf = zeros(128,8,8);
                for iTx = 1:8
                    ctf(:,:,iTx) = util.fixctf((yf(:,:,iTx)./sf));
                        % ./b2bData(iLink).ctf(:,:,iTx));
                end
                powLinkTmp = pow2db(abs(ctf).^2);
                powLink(:,:,iLink) =  squeeze(mean(powLinkTmp));
                iLink = iLink+1;
            end
        end
    end


    Logger.info("send command to stop transmit")
    node(txNode).tcpSock.write("2")  %end tx
    tcpData = waittcpdata(node(txNode).tcpSock);
    Logger.info("TCP message received :"+tcpData)
    if(tcpData ~= '2')
        Logger.error("Unexpected Error")
    end
    Logger.info("All Link Measurement Done!")
    if(cb.Value)
        for iLink = 1:3
            heatmap(panel(iLink),powLink(:,:,iLink), ...
                "ColorLimits",[-60 -30], "Colormap",jet)
            panel(iLink).Title = "Link: " + iLink;
        end
    end
end

clear node
clear udpSock
close(fig)


function tcpData = waittcpdata(sock)
while(true)
    numBytesAvailable = sock.NumBytesAvailable;
    if(logical(numBytesAvailable))
        tcpData = sock.read(numBytesAvailable,"char");
        break
    end
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

