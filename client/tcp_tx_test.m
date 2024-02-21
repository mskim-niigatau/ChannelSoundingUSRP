clear
close all

ADDR = "127.0.0.1";
UDP_PORT = 12345;
TCP_PORT = 54321;

freq = 4.85e9;
rate = 200e6;
ref = "internal";%external, internal, gpsdo

txFile = fullfile(pwd,"data","multitone_x7.dat");
execPath = fullfile(pwd,"..","txrx_core","build","txrx_core");

nSampsPerOnce = 256;
nTxPort = 8;
nRxPort = 8;
nSampsTotal = nSampsPerOnce * nTxPort * nRxPort * 2;
nDelayTotal = 256*8*8*2;    %txrx_coreの仕様上，送信信号と同期を取るためnSampsPerOnce(=256)の倍数である必要があります


Logger = util.LoggerClass;

%% setup udp and tcp socket

args = sprintf("--tx-file %s --freq %.3fe9 --rate %de6 --lo_off -1 --tx-gain 25 --rx-gain 37.5" + ...
    " --rx-ant TX/RX --ref %s --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s", ...
    txFile, freq/1e9, rate/1e6, ref, nSampsPerOnce, nTxPort ,nRxPort, nDelayTotal, string(TCP_PORT));
if isunix
    clipboard('copy', execPath + " " + args + " && exit")
    system("gnome-terminal")
    answer = questdlg('To continue, open a terminal, paste the contents of the clipboard and run it.', ...
        'Waiting for...', ...
        'Done','Cancel','Done');
    if ~isequal(answer,'Done')
        return
    end
else
    system(execPath + ".exe " + args + " & exit &");
    pause(20)
end

udpSock = udpport("LocalHost",ADDR,"LocalPort",UDP_PORT,"Timeout",0.1);
udpSock.flush
tcpSock = tcpclient(ADDR,TCP_PORT);

%% data acquisition
udpSock.Timeout = 1;
ctf = zeros(nSampsPerOnce,nRxPort,nTxPort);

if(waittcpdata(tcpSock) ~= '0')
    Logger.info("tcpData = "+tcpData)
    clear("tcpSock")
    return
end

tcpSock.write("1")
if(waittcpdata(tcpSock) == '1')
    Logger.info("Tx started")
else
    Logger.error("Tx start failed")
    clear("tcpSock")
    udpSock.flush
    udpSock.delete
    return
end

msgfig = msgbox('Now Streaming...','Tx','modal');
uiwait(msgfig)

tcpSock.write('2')
if(waittcpdata(tcpSock) == '2')
    Logger.info("Tx finished")
else
    Logger.error("Tx finish failed")
end

pause(1)
clear("tcpSock")

udpSock.flush
udpSock.delete

function tcpData = waittcpdata(sock)
while(true)
    numBytesAvailable = sock.NumBytesAvailable;
    if(logical(numBytesAvailable))
        tcpData = sock.read(numBytesAvailable,"char");
        break
    end
end
end

