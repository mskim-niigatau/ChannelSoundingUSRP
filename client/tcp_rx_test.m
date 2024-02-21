clear
close all

ADDR = "127.0.0.1";
UDP_PORT = 12345;
TCP_PORT = 54321;

Logger = util.LoggerClass;

freq = 4.85e9;
rate = 200e6;
bw = rate/2;
nMean = 1;
nSampsPerOnce = 256;
nSampsTotal = nSampsPerOnce * 8 * 8 * 2;
nDelayTotal = nSampsTotal;
nTone = nSampsPerOnce/2;
deltaF = bw/nTone;
freqRange = freq-bw/2:deltaF:freq+bw/2-deltaF;

%% setup udp and tcp socket
Logger.info("Start RX program...")
args = sprintf("--freq %.3fe9 --rate %de6 --lo_off -1 --rx-gain 37.5" + ...
    " --ref external --samps %d --tx-ports %d --rx-ports %d --delay %d" + ...
    " --tcp-port %s --repeat --tx-file data/multitone.dat", ...
    freq/1e9, rate/1e6, nSampsPerOnce, 8 ,8, nDelayTotal, string(TCP_PORT));
system("..\txrx_core\build\txrx_core.exe " + args + " & exit &");

pause(20)
Logger.info("Attempt to connect the UDP socket...")
udpSock = udpport("LocalHost",ADDR,"LocalPort",UDP_PORT,"Timeout",0.1);
Logger.info("Successfully connected UDP socket!")
udpSock.flush
Logger.info("Attempt to connect the TCP socket...")
tcpSock = tcpclient(ADDR,TCP_PORT);
Logger.info("Successfully connected TCP socket!")

%% data acquisition
s = util.readcplxfile("data/multitone.dat");
sf = fft(s);
udpSock.Timeout = 1;
ctf = zeros(nSampsPerOnce,8,8);

tcpData = '';
while(isempty(tcpData))
    tcpData = char(tcpSock.read());
    pause(.01)
end
if(tcpData(1) ~= '0')
    Logger.info("tcpData = "+tcpData)
    clear("tcpSock")
    return
end


% Calibration data
caliKitData = load("data\Hc_4_8GHz.mat");
hcB2b = caliKitData.Hc;
b2bData = load("recieve_data\b2b\1.mat");
ctfB2b = b2bData.ctf./hcB2b;

Logger.info("Start data acquisition")
yt = zeros(nSampsPerOnce,8,8);
iData = 0;
sigHandle = figure;
ctfHandle = figure;
while(iData < nMean)
    udpSock.flush;
    tcpSock.write("3")
    tcpData = '';
    while(isempty(tcpData))
        tcpData = char(tcpSock.read());
        pause(.01)

    end
    if(tcpData(1) == '4')
        continue
    elseif (tcpData(1) ~= '3')
        break
    end

    data = udpSock.read(nSampsTotal*2,"single");
    data = util.tocplx(data);
    ytAll = data;
    yt = reshape(ytAll,nSampsPerOnce*2,8,8);
    yt(1:nSampsPerOnce,:,:) = [];

    % plot tmp data
    figure(sigHandle)
    tiledlayout(4,2)
    for jTx = 1:8
        nexttile
        plot(real(yt(:,:,jTx)))
        title("Tx: " + jTx)
        ylim([-0.05 0.05])
    end

    yf = fft(yt);
    figure(ctfHandle)
%     tiledlayout(4,2)
    ctf = zeros(nSampsPerOnce/2,8,8);
    for jTx = 1:8
        ctf(:,:,jTx) = util.fixctf((yf(:,:,jTx)./sf)./ctfB2b(:,:,jTx));
%         nexttile
%         plot(freqRange/1e6, pow2db(fftshift(abs(ctf(:,:,jTx)).^2)))
%         title("Tx: " + jTx)
    end
    powLink = pow2db(abs(ctf).^2);
    powLink =  squeeze(mean(powLink));
    h = heatmap(powLink);
    h.ColorLimits = [-80 -30];
    h.Colormap = jet;
    xlabel("Tx")
    ylabel("Rx")
    drawnow


    iData = iData+1;
end
Logger.info("Done!")
clear("tcpSock")

udpSock.flush
udpSock.delete

