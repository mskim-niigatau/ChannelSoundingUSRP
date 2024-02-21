%#ok<*SAGROW> 

clear
close all

ADDR = "127.0.0.1";
UDP_PORT = 12345;
TCP_PORT = 54321;

freq = 2.425e9;
rate = 50e6;
nRecv = 10;
nDelay = 50;
nSampsPerOnce = 256;
nTxPort = 8;
nRxPort = 8;
nSampsTotal = nSampsPerOnce * nTxPort * nRxPort * 2;
nDelayTotal = nDelay * nSampsPerOnce*2*8*8;

sync = "external";

%% setup udp and tcp socket
args = sprintf("--freq %.3fe9 --rate %de6 --lo_off -1 --gain 10" + ...
    " --ref %s --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s --repeat", ...
    freq/1e9, rate/1e6, sync, nSampsPerOnce, ...
    nTxPort ,nRxPort, nDelayTotal, string(TCP_PORT));
system("..\rx_core\build\rx_core.exe " + args + " & exit &");

pause(1)
udpSock = udpport("LocalHost",ADDR,"LocalPort",UDP_PORT,"Timeout",0.1);
udpSock.flush
tcpSock = tcpclient(ADDR,TCP_PORT);

%%
udpSock.Timeout = 1;
recvData = {};

tcpData = "";
while(tcpData == "")
    tcpData = string(char(tcpSock.read()));
    pause(.01)
end
if(tcpData ~= "1")
    clear("tcpSock")
    return
end

iData = 0;
ytMean = zeros(256,8,8);
while(iData < nRecv)
    udpSock.flush;
    tcpSock.write("recv")    
    tcpData = "";
    while(tcpData == "")
        tcpData = string(char(tcpSock.read()));
        pause(.01)
    end
    if(tcpData == "0")
        continue
    elseif (tcpData ~= "1")
        break
    end

    data = udpSock.read(nSampsTotal*2,"single");
    data = util.tocplx(data);
    ytAll = data;
    yt = reshape(ytAll,nSampsPerOnce*2,nRxPort,nTxPort);
    yt(1:nSampsPerOnce,:,:) = [];
    yf = fft(yt);
    ytMean = ytMean + yt;

    recvData{iData+1}.yt = yt;
    recvData{iData+1}.yf = yf;  

    tiledlayout(nTxPort/2,2)
    for iTx = 1:nTxPort
        nexttile
        ytTmp = squeeze(yt(:,:,iTx));
        plot(real(ytTmp))
        xlim([0 nSampsPerOnce])
        ylim([-.5 .5])
        title("Tx. "+iTx)
    end
    iData = iData+1;
end
ytMean = ytMean./nRecv;
yfMean = fft(ytMean);
clear("tcpSock")
udpSock.flush
udpSock.delete