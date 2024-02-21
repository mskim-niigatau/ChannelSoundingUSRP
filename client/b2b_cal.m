clear
close all

ADDR = "127.0.0.1";
UDP_PORT = 12345;
TCP_PORT = 54321;

Logger = util.LoggerClass;

freq = 4.85e9;
rate = 200e6;
nMean = 10;
nSampsPerOnce = 256;
nSampsTotal = nSampsPerOnce * 8 * 8 * 2;
nDelayTotal = nSampsTotal;

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

warning('off','daq:Session:onDemandOnlyChannelsAdded')
CalKitAtt = daq("ni");
CalKitAtt.addoutput("Dev1","port1/line0:4","Digital")
CalKitAtt.write([1 1 1 1 1])
CalKitController = daq("ni");
CalKitController.addoutput("Dev1","port0/line0:2","Digital")
warning('on','daq:Session:onDemandOnlyChannelsAdded')

Logger.info("Start data acquisition")
yt = zeros(nSampsPerOnce,8,8);
for iTx = 1:8
    Logger.info("Tx Port: " + iTx)
    chData = flip(~decimalToBinaryVector(iTx,4));
    CalKitController.write(chData(1:3))
    pause(.1)
    iData = 0;
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
        ytTmp = reshape(ytAll,nSampsPerOnce*2,8,8);
        ytTmp(1:nSampsPerOnce,:,:) = [];
        yt(:,:,iTx) = yt(:,:,iTx) + ytTmp(:,:,iTx);

        % plot tmp data
        tiledlayout(4,2)
        for jTx = 1:8
            nexttile
            plot(real(ytTmp(:,:,jTx)))
            title("Tx: " + jTx)
        end

        iData = iData+1;
    end
end
Logger.info("Done!")
clear("tcpSock")

udpSock.flush
udpSock.delete

for iTx = 1:8
    yt(:,:,iTx) = yt(:,:,iTx)./nMean;
end
yf = fft(yt);
ctf = yf./sf;

%% save data
[FILE_NAME, FILE_DIR] = uiputfile("*.mat");
if FILE_NAME
    save(fullfile(FILE_DIR,FILE_NAME),"ctf")
end
close