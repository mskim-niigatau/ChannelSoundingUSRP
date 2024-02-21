% Tx/Rx 切り替えテストプログラム
% 2023/09/25

% Description : 受信，送信の順でnMeasureの数だけ繰り返し動作を行います

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

nMeasure = 10;

%% setup udp and tcp socket

args = sprintf("--tx-file %s --freq %.3fe9 --rate %de6 --lo_off -1 --tx-gain 20 --rx-gain 15" + ...
    " --rx-ant TX/RX --ref %s --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s --repeat", ...
    txFile, freq/1e9, rate/1e6, ref, nSampsPerOnce, nTxPort ,nRxPort, nDelayTotal, string(TCP_PORT));
if isunix
    clipboard('copy', execPath + " " + args)
    answer = questdlg('To continue, open a terminal, paste the contents of the clipboard and run it.', ...
        'Waiting for...', ...
        'Done','Cancel','Done');
    if ~isequal(answer,'Done')
        return
    end
else
    system(execPath + ".exe " + args + " & exit &");
end

pause(1)
udpSock = udpport("LocalHost",ADDR,"LocalPort",UDP_PORT,"Timeout",0.1);
udpSock.flush
tcpSock = tcpclient(ADDR,TCP_PORT);

%% data acquisition
udpSock.Timeout = 1;
ctf = zeros(nSampsPerOnce,nRxPort,nTxPort);

tcpData = "";
while(tcpData == "")
    tcpData = string(char(tcpSock.read()));
    pause(.01)
end
if(tcpData ~= "1")
    clear("tcpSock")
    return
end

phaseChange = zeros(nMeasure,1);
time = zeros(nMeasure,1);
iData = 0;
tic
while(iData < nMeasure)
    % Rx phase
    clear ytAll yt yf

    udpSock.flush;
    tcpSock.write("r")
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

    ytAll = util.tocplx(udpSock.read(nSampsTotal*2,"single"));
    yt = reshape(ytAll,nSampsPerOnce*2,nRxPort,nTxPort);
    yt(1:nSampsPerOnce,:,:) = [];
    yf = fft(yt);

    time(iData+1) = toc;

    tiledlayout(3,1);
    nexttile
    plot(real(ytAll))
    title('ytAll')
    xlabel('samples')
    nexttile
    plot(real(yt(:,1,1)))
    title('yt')
    xlabel('samples')
    nexttile
    plot(fftshift(pow2db(abs(yf(:,1,1)).^2)))
    title('yf')
    ylabel('[dB]')
    xlabel('samples')
    
    % Tx phase
    udpSock.flush;
    tcpSock.write("t")
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

    iData = iData+1;
end

clear("tcpSock")

udpSock.flush
udpSock.delete

