// Qt Quick OpenGL + Qt WebEngine leak tester.
//
// Without arguments: runs itself once per scene graph backend (opengl, d3d11),
// each child shows a page that re-renders every second and samples its own
// memory, then writes report.txt next to the executable.
//
// leaktest.exe [--seconds N]           run both backends (default 60 s each)
// leaktest.exe --child <backend> <N>   internal: one measured run

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <rhi/qrhi.h>

#include <windows.h>
#include <dxgi.h>
#include <psapi.h>

#include <cstdio>

static const int WARMUP_SECONDS = 10;
static const int SAMPLE_SECONDS = 5;
static const double LEAK_THRESHOLD_MB = 100.0; // per minute; d3d11 stays within ~15 MB

static void out(const QString& line)
{
  std::fputs(qPrintable(line + "\n"), stdout);
  std::fflush(stdout);
}

static qint64 privateBytes()
{
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<qint64>(pmc.PrivateUsage);
}

static int handleCount()
{
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  return static_cast<int>(count);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Child: one measured run with the given backend
static int runChild(int argc, char** argv, const QString& backend, int seconds)
{
  QQuickWindow::setGraphicsApi(backend == "opengl" ? QSGRendererInterface::OpenGL
                                                   : QSGRendererInterface::Direct3D11);
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QtWebEngineQuick::initialize();
  QGuiApplication app(argc, argv);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("backendName", backend);
  engine.rootContext()->setContextProperty("secondsLeft", WARMUP_SECONDS + seconds);
  engine.load(QUrl("qrc:/main.qml"));
  if (engine.rootObjects().isEmpty())
  {
    out("ERROR failed to load QML");
    return 1;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());

  int elapsed = 0;
  qint64 baseMem = 0;
  int baseHandles = 0;
  bool reportedRhi = false;

  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    elapsed++;
    engine.rootContext()->setContextProperty("secondsLeft", WARMUP_SECONDS + seconds - elapsed);

    if (!reportedRhi && window && window->rhi())
    {
      QRhiDriverInfo info = window->rhi()->driverInfo();
      out(QString("RHI %1 | %2 | vendor 0x%3 device 0x%4")
            .arg(QString::fromLatin1(window->rhi()->backendName()), QString::fromUtf8(info.deviceName))
            .arg(info.vendorId, 0, 16).arg(info.deviceId, 0, 16));
      reportedRhi = true;
    }

    if (elapsed == WARMUP_SECONDS)
    {
      baseMem = privateBytes();
      baseHandles = handleCount();
    }
    int t = elapsed - WARMUP_SECONDS;
    if (t >= 0 && t % SAMPLE_SECONDS == 0)
      out(QString("SAMPLE %1 %2 %3").arg(t).arg(privateBytes()).arg(handleCount()));
    if (t >= seconds)
    {
      out(QString("RESULT %1 %2 %3 %4").arg(baseMem).arg(privateBytes()).arg(baseHandles).arg(handleCount()));
      app.quit();
    }
  });
  timer.start(1000);
  return app.exec();
}

/////////////////////////////////////////////////////////////////////////////////////////
// Controller: system info, one child per backend, report
static QStringList gpuInfo()
{
  QStringList lines;
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    return {"(DXGI unavailable)"};

  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    QString driver = "?";
    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
      driver = QString("%1.%2.%3.%4").arg(HIWORD(umd.HighPart)).arg(LOWORD(umd.HighPart))
                 .arg(HIWORD(umd.LowPart)).arg(LOWORD(umd.LowPart));
    lines << QString("%1 (vendor 0x%2 device 0x%3) driver %4, %5 MB dedicated%6")
               .arg(QString::fromWCharArray(desc.Description))
               .arg(desc.VendorId, 4, 16, QChar('0')).arg(desc.DeviceId, 4, 16, QChar('0'))
               .arg(driver).arg(desc.DedicatedVideoMemory / (1024 * 1024))
               .arg(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " [software]" : "");
    adapter->Release();
  }
  factory->Release();
  return lines;
}

struct RunResult
{
  QString backend;
  QString rhi;
  QStringList samples;
  double growthMb = 0;
  int handleGrowth = 0;
  bool ok = false;
  QString error;
};

static RunResult runBackend(const QString& backend, int seconds)
{
  RunResult r;
  r.backend = backend;
  out(QString("\n=== %1: %2 s warm-up + %3 s measurement ===").arg(backend).arg(WARMUP_SECONDS).arg(seconds));

  QProcess child;
  child.setProcessChannelMode(QProcess::SeparateChannels);
  child.start(QCoreApplication::applicationFilePath(),
              {"--child", backend, QString::number(seconds)});
  if (!child.waitForStarted())
  {
    r.error = "could not start child process";
    return r;
  }

  QByteArray pending;
  auto handleLine = [&](const QString& line) {
    if (line.startsWith("RHI "))
    {
      r.rhi = line.mid(4);
      out("  renderer: " + r.rhi);
    }
    else if (line.startsWith("SAMPLE "))
    {
      QStringList f = line.split(' ');
      QString s = QString("  %1 s  private %2 MB  handles %3")
                    .arg(f[1], 4).arg(f[2].toLongLong() / (1024 * 1024), 6).arg(f[3]);
      r.samples << s;
      out(s);
    }
    else if (line.startsWith("RESULT "))
    {
      QStringList f = line.split(' ');
      r.growthMb = (f[2].toLongLong() - f[1].toLongLong()) / (1024.0 * 1024.0);
      r.handleGrowth = f[4].toInt() - f[3].toInt();
      r.ok = true;
    }
    else if (line.startsWith("ERROR "))
    {
      r.error = line.mid(6);
    }
  };

  QDeadlineTimer deadline((WARMUP_SECONDS + seconds + 60) * 1000);
  while (child.state() != QProcess::NotRunning && !deadline.hasExpired())
  {
    child.waitForReadyRead(500);
    pending += child.readAllStandardOutput();
    int nl;
    while ((nl = pending.indexOf('\n')) >= 0)
    {
      handleLine(QString::fromUtf8(pending.left(nl)).trimmed());
      pending.remove(0, nl + 1);
    }
  }
  if (child.state() != QProcess::NotRunning)
  {
    child.kill();
    child.waitForFinished();
    if (!r.ok)
      r.error = "timed out";
  }
  else if (!r.ok && r.error.isEmpty())
  {
    r.error = QString("child exited early (exit code %1, %2)")
                .arg(child.exitCode())
                .arg(child.exitStatus() == QProcess::CrashExit ? "crashed" : "normal exit");
  }
  return r;
}

static int runController(int argc, char** argv, int seconds)
{
  QCoreApplication app(argc, argv);

  QStringList report;
  report << "Qt Quick OpenGL + Qt WebEngine leak test"
         << "Date:    " + QDateTime::currentDateTime().toString(Qt::ISODate)
         << "OS:      " + QSysInfo::prettyProductName() + " (kernel " + QSysInfo::kernelVersion() + ", "
                + QSysInfo::currentCpuArchitecture() + ")"
         << "Qt:      " + QString::fromLatin1(qVersion())
         << "GPUs:";
  for (const QString& gpu : gpuInfo())
    report << "  " + gpu;
  for (const QString& line : report)
    out(line);

  QList<RunResult> results;
  for (const QString& backend : {QString("opengl"), QString("d3d11")})
    results << runBackend(backend, seconds);

  report << "" << QString("Each run: %1 s warm-up, then %2 s measurement; the page re-renders every second.")
                    .arg(WARMUP_SECONDS).arg(seconds);
  bool anyLeak = false;
  QStringList failed;
  for (const RunResult& r : results)
  {
    report << "" << "[" + r.backend + "]";
    if (!r.rhi.isEmpty())
      report << "  renderer: " + r.rhi;
    report << r.samples;
    if (!r.ok)
    {
      report << "  ERROR: " + r.error;
      failed << r.backend;
      continue;
    }
    bool leak = r.growthMb > LEAK_THRESHOLD_MB * seconds / 60.0;
    anyLeak |= leak;
    report << QString("  growth: %1 MB, %2 handles  ->  %3")
                .arg(r.growthMb, 0, 'f', 0).arg(r.handleGrowth).arg(leak ? "LEAK" : "ok");
  }
  // A failed run cannot show a leak, so it must not count as "no leak"
  QString verdict = anyLeak ? "LEAK DETECTED"
                    : failed.isEmpty() ? "no leak detected"
                                       : "INCONCLUSIVE (failed: " + failed.join(", ") + ")";
  report << "" << "Verdict: " + verdict;

  QString path = QDir(QCoreApplication::applicationDirPath()).filePath("report.txt");
  QFile file(path);
  if (file.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    QTextStream(&file) << report.join("\n") << "\n";
    out("\n" + report.last() + "\nReport written to " + path);
  }
  else
  {
    out("\n" + report.last() + "\nCould not write " + path + ", full report:\n" + report.join("\n"));
  }
  return 0;
}

int main(int argc, char** argv)
{
  if (argc >= 4 && QString(argv[1]) == "--child")
    return runChild(argc, argv, QString(argv[2]), QString(argv[3]).toInt());

  int seconds = 60;
  if (argc >= 3 && QString(argv[1]) == "--seconds")
    seconds = qMax(10, QString(argv[2]).toInt());
  return runController(argc, argv, seconds);
}
