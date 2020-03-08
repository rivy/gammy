/**
 * Copyright (C) 2019 Francesco Fusco. All rights reserved.
 * License: https://github.com/Fushko/gammy#license
 */

#ifdef _WIN32
	#include "dxgidupl.h"
	#pragma comment(lib, "gdi32.lib")
	#pragma comment(lib, "user32.lib")
	#pragma comment(lib, "DXGI.lib")
	#pragma comment(lib, "D3D11.lib")
	#pragma comment(lib, "Advapi32.lib")
#else
	#include <signal.h>
#endif

#include "cfg.h"
#include "utils.h"

#include <thread>
#include <mutex>
#include <chrono>
#include <QApplication>
#include <QTime>
#include <algorithm>
#include "mainwindow.h"

// Reflects the current screen brightness
int brt_step = brt_slider_steps;

#ifndef _WIN32
// Pointers for quitting normally in signal handler
static bool   *p_quit;
static convar *p_ss_cv;
static convar *p_temp_cv;
#endif

void adjustTemperature(convar &temp_cv, MainWindow &w)
{
	using namespace std::this_thread;
	using namespace std::chrono;
	using namespace std::chrono_literals;

	const auto setTime = [] (QTime &t, const std::string &time_str)
	{
		const auto start_hour = time_str.substr(0, 2);
		const auto start_min  = time_str.substr(3, 2);

		t = QTime(std::stoi(start_hour), std::stoi(start_min));
	};

	enum TempState {
		HIGH,
		INCREASING,
		LOWERING,
		LOW
	};

	bool start_r;
	bool end_r;

	bool force = false;
	w.force_temp_change = &force;

	QTime     time_start;
	QTime     time_end;
	QDateTime datetime_start;
	QDateTime datetime_end;

	int64_t jday_start;
	int64_t jday_end;

	const auto setDates = [&]
	{
		setTime(time_start, cfg["time_start"]);
		setTime(time_end, cfg["time_end"]);

		datetime_start = QDateTime(QDate::fromJulianDay(jday_start), time_start);
		datetime_end   = QDateTime(QDate::fromJulianDay(jday_end), time_end);

		LOGD << "Dates set";
	};

	const auto checkDates = [&]
	{
		const auto now = QDateTime::currentDateTime();
		const auto end = now.addSecs(3600 * 24);

		start_r = now > datetime_start;
		end_r   = start_r ? now > datetime_end : false;

		LOGV << "Start: " << datetime_start.toString() << " reached: " << start_r;
		LOGV << "End: " << datetime_end.toString() << " reached: " << end_r;
	};

	const auto resetInterval = [&] (bool shift_days = false)
	{
		if(shift_days)
		{
			++jday_start;
			++jday_end;
		}

		setDates();
		checkDates();
	};

	int64_t today    = QDate::currentDate().toJulianDay();
	int64_t tomorrow = today + 1;

	jday_start = today;
	jday_end   = tomorrow;

	setDates();
	checkDates();

	bool fast_change = true;

	LOGD << "Starting temp state: " << cfg["temp_state"];

	if(cfg["temp_state"] == LOW)
	{
		if(!start_r)
		{
			jday_end = today;
			resetInterval();
		}
	}

	std::mutex temp_mtx;

	bool needs_change = cfg["auto_temp"];

	convar     clock_cv;
	std::mutex clock_mtx;

	std::thread clock ([&]
	{
		while(true)
		{
			{
				std::unique_lock<std::mutex> lk(clock_mtx);
				clock_cv.wait_until(lk, system_clock::now() + 1s, [&] { return w.quit; });
			}

			if(w.quit) break;

			if(!cfg["auto_temp"]) continue;

			checkDates();

			{
				std::lock_guard<std::mutex> lock(temp_mtx);

				bool low_period  = start_r && !end_r;
				bool high_period = !start_r && end_r;

				if(low_period)
				{
					cfg["temp_state"] = LOWERING;
					needs_change = true;
				}
				else if(high_period)
				{
					cfg["temp_state"] = INCREASING;
					needs_change = true;
				}

				fast_change  = false;
			}

			temp_cv.notify_one();
		}
	});

	while (true)
	{
		// Lock
		{
			std::unique_lock<std::mutex> lock(temp_mtx);

			temp_cv.wait(lock, [&]
			{
				return needs_change || force || w.quit;
			});

			if(w.quit) break;

			if(force)
			{
				resetInterval();

				if(!start_r && (cfg["temp_state"] == LOWERING || cfg["temp_state"] == LOW))
				{
					cfg["temp_state"] = INCREASING;
					fast_change = true;
				}

				if(start_r && (cfg["temp_state"] == HIGH || cfg["temp_state"] == INCREASING))
				{
					cfg["temp_state"] = LOWERING;
					fast_change = true;
				}

				/*if(cfg["temp_state"] == LOW || cfg["temp_state"] == LOWERING)
				{
					if(!start_r && jday_end != today)
					{
						LOGD << "Forcing high temp";

						cfg["temp_state"] = INCREASING;
						fast_change = true;
					}
				}

				if(cfg["temp_state"] == INCREASING || cfg["temp_state"] == LOWERING) fast_change = false;*/

				force = false;
			}
			else
			{
				needs_change = false;
			}
		}

		if(!cfg["auto_temp"]) continue;

		/*if(cfg["temp_state"] == INCREASING || cfg["temp_state"] == HIGH)
		{
			LOGD << "We need to increase.";

			if(start_r)
			{
				LOGD << "Start date reached.";
				cfg["temp_state"] = LOWERING;
			}
		}
		else if (cfg["temp_state"] == LOWERING || cfg["temp_state"] == LOW)
		{
			LOGD << "We need to lower.";

			if(start_r && end_r)
			{
				LOGD << "Start and end reached. Shifting days.";
				resetInterval(true);

				cfg["temp_state"] = INCREASING;
			}
			else if(jday_end == today && end_r)
			{
				// We get here if the app was started on low temp
				// but the start date hasn't been reached

				LOGD << "End date reached. Shifting end date.";

				cfg["temp_state"] = INCREASING;

				jday_end++;
				resetInterval();
			}
			else cfg["temp_state"] = INCREASING;
		}*/

		const int target_temp = cfg["temp_state"] == INCREASING ? cfg["temp_high"] : cfg["temp_low"];
		const int target_step = int(remap(target_temp, min_temp_kelvin, max_temp_kelvin, temp_slider_steps, 0));

		LOGD << "Target temp: " << target_temp << " K";

		int cur_step = cfg["temp_step"];

		if(target_step == cur_step)
		{
			LOGD << "Temperature is already at target.";
			continue;
		}

		const int start = cur_step;
		const int end   = target_step;

		// @TODO: Remove this
		if(!cfg["temp_speed"].get_ptr<json::number_float_t*>()) cfg["temp_speed"] = 30.;

		double min = cfg["temp_speed"];

		double duration = fast_change ? (2) : (min * 60);

		fast_change = false;

		const double iterations = FPS * duration;
		const int distance      = end - start;
		const double time_incr  = duration / iterations;

		double time = 0;

		const auto adjusted = [&]
		{
			time += time_incr;
			cfg["temp_step"] = int(easeInOutQuad(time, start, distance, duration));

			w.setTempSlider(cfg["temp_step"]);

			return cfg["temp_step"] == end;
		};

		LOGD << "Adjusting...";

		while (cfg["auto_temp"])
		{
			if(w.quit) break;

			if(force)
			{
				resetInterval();

				if(!start_r && cfg["temp_state"] == LOWERING)
				{
					cfg["temp_state"] = INCREASING;
					fast_change = true;
					break;
				}

				if(start_r && cfg["temp_state"] == INCREASING)
				{
					cfg["temp_state"] = LOWERING;
					fast_change = true;
					break;
				}

				force = false;
			}

			if(adjusted())
			{
				LOGD << "Temp adjustment done.";

				cfg["temp_state"] = cfg["temp_state"] == INCREASING ? HIGH : LOW;
				break;
			}

			sleep_for(milliseconds(1000 / FPS));
		}
	}

	LOGD << "Notifying clock thread";

	clock_cv.notify_one();
	clock.join();

	LOGD << "Clock thread joined";
}

struct Args
{
	convar br_cv;
	std::mutex br_mtx;

#ifndef _WIN32
	X11 *x11 {};
#endif

	int img_br = 0;
	bool br_needs_change = false;
};

void adjustBrightness(Args &args, MainWindow &w)
{
	using namespace std::this_thread;
	using namespace std::chrono;

	while(true)
	{
		int img_br;

		{
			std::unique_lock<std::mutex> lock(args.br_mtx);

			args.br_cv.wait(lock, [&]
			{
				return args.br_needs_change;
			});

			if(w.quit) break;

			args.br_needs_change = false;

			img_br = args.img_br;
		}

		int target = brt_slider_steps - int(remap(img_br, 0, 255, 0, brt_slider_steps)) + cfg["offset"].get<int>();
		target = std::clamp(target, cfg["min_br"].get<int>(), cfg["max_br"].get<int>());

		if (target == brt_step)
		{
			LOGD << "Brightness is already at target.";
			continue;
		}

		const int start = brt_step;
		const int end   = target;

		double duration = cfg["speed"];

		const double iterations = FPS * duration;
		const int distance      = end - start;
		const double time_incr  = duration / iterations;

		double time = 0;

		const auto adjust = [&]
		{
			time += time_incr;
			brt_step = int(std::ceil(easeOutExpo(time, start, distance, duration)));

			w.setBrtSlider(brt_step);

			return brt_step == target;
		};

		while (!args.br_needs_change && cfg["auto_br"])
		{
			if(w.quit) break;

			if(adjust())
			{
				LOGD << "Brt adjustment done.";
				break;
			}

			sleep_for(milliseconds(1000 / FPS));
		}
	}
}

void recordScreen(Args &args, convar &ss_cv, MainWindow &w)
{
	using namespace std::this_thread;
	using namespace std::chrono;

	LOGV << "recordScreen() start";

#ifdef _WIN32
	const uint64_t width	= GetSystemMetrics(SM_CXVIRTUALSCREEN) - GetSystemMetrics(SM_XVIRTUALSCREEN);
	const uint64_t height	= GetSystemMetrics(SM_CYVIRTUALSCREEN) - GetSystemMetrics(SM_YVIRTUALSCREEN);
	const uint64_t len	= width * height * 4;

	LOGD << "Screen resolution: " << width << '*' << height;

	DXGIDupl dx;

	bool useDXGI = dx.initDXGI();

	if (!useDXGI)
	{
		LOGE << "DXGI initialization failed. Using GDI instead";
		w.setPollingRange(1000, 5000);
	}
#else
	const uint64_t screen_res = args.x11->getWidth() * args.x11->getHeight();
	const uint64_t len = screen_res * 4;

	args.x11->setXF86Gamma(brt_step, cfg["temp_step"]);
#endif

	LOGD << "Buffer size: " << len;

	// Buffer to store screen pixels
	std::vector<uint8_t> buf;

	std::thread br_thr(adjustBrightness, std::ref(args), std::ref(w));

	const auto getSnapshot = [&] (std::vector<uint8_t> &buf)
	{
		LOGV << "Taking screenshot";

#ifdef _WIN32
		if (useDXGI)
		{
			while (!dx.getDXGISnapshot(buf)) dx.restartDXGI();
		}
		else
		{
			getGDISnapshot(buf);
			sleep_for(milliseconds(cfg["polling_rate"]));
		}
#else
		args.x11->getX11Snapshot(buf);

		sleep_for(milliseconds(cfg["polling_rate"]));
#endif
	};

	std::mutex m;

	int img_delta = 0;

	bool force = false;

	int
	prev_img_br	= 0,
	prev_min	= 0,
	prev_max	= 0,
	prev_offset	= 0;

	while (true)
	{
		{
			std::unique_lock<std::mutex> lock(m);

			ss_cv.wait(lock, [&]
			{
				return cfg["auto_br"] || w.quit;
			});
		}

		if(w.quit)
		{
			break;
		}

		if(cfg["auto_br"])
		{
			buf.resize(len);
			force = true;
		}
		else
		{
			buf.clear();
			buf.shrink_to_fit();
			continue;
		}

		while(cfg["auto_br"] && !w.quit)
		{
			getSnapshot(buf);

			const int img_br = calcBrightness(buf);
			img_delta += abs(prev_img_br - img_br);

			if (img_delta > cfg["threshold"] || force)
			{
				img_delta = 0;
				force = false;

				{
					std::lock_guard lock (args.br_mtx);

					args.img_br = img_br;
					args.br_needs_change = true;
				}

				args.br_cv.notify_one();
			}

			if (cfg["min_br"] != prev_min || cfg["max_br"] != prev_max || cfg["offset"] != prev_offset)
			{
				force = true;
			}

			prev_img_br	= img_br;
			prev_min	= cfg["min_br"];
			prev_max	= cfg["max_br"];
			prev_offset	= cfg["offset"];
		}

		buf.clear();
		buf.shrink_to_fit();
	}

	LOGD << "Exited screenshot loop. Notifying adjustBrightness";

	{
		std::lock_guard<std::mutex> lock (args.br_mtx);
		args.br_needs_change = true;
	}

	args.br_cv.notify_one();

	br_thr.join();

	LOGD << "adjustBrightness joined";

	LOGD << "Notifying QApplication";

	QApplication::quit();
}

void sig_handler(int signo);

int main(int argc, char **argv)
{
	static plog::RollingFileAppender<plog::TxtFormatter> file_appender("gammylog.txt", 1024 * 1024 * 5, 1);
	static plog::ColorConsoleAppender<plog::TxtFormatter> console_appender;

	plog::init(plog::Severity(plog::debug), &console_appender);

	read();

	if(!cfg["auto_br"].get<bool>())
	{
		// Start with manual brightness setting, if auto brightness is disabled
		LOGD << "Autobrt OFF. Setting manual brt step.";
		brt_step = cfg["brightness"];
	}

	if(cfg["auto_temp"].get<bool>())
	{
		LOGD << "Autotemp ON. Starting from step 0."; // To allow smooth transition
		cfg["temp_step"] = 0;
	}

	plog::get()->addAppender(&file_appender);
	plog::get()->setMaxSeverity(plog::Severity(cfg["log_lvl"]));

#ifdef _WIN32
	checkInstance();
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

	if(cfg["log_lvl"] == plog::verbose)
	{
		FILE *f1, *f2, *f3;
		AllocConsole();
		freopen_s(&f1, "CONIN$", "r", stdin);
		freopen_s(&f2, "CONOUT$", "w", stdout);
		freopen_s(&f3, "CONOUT$", "w", stderr);
	}

	checkGammaRange();
#else
	signal(SIGINT, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGTERM, sig_handler);

	X11 x11;
#endif

	QApplication a(argc, argv);

	convar ss_cv;
	convar temp_cv;
	Args thr_args;

#ifdef _WIN32
	MainWindow wnd(nullptr, &ss_cv, &temp_cv);
#else
	MainWindow wnd(&x11, &ss_cv, &temp_cv);

	thr_args.x11 = &x11;
	p_quit = &wnd.quit;
	p_ss_cv = &ss_cv;
	p_temp_cv = &temp_cv;
#endif

	std::thread temp_thr(adjustTemperature, std::ref(temp_cv), std::ref(wnd));
	std::thread ss_thr(recordScreen, std::ref(thr_args), std::ref(ss_cv), std::ref(wnd));


	a.exec();

	LOGD << "QApplication joined";

	temp_thr.join();

	LOGD << "adjustTemperature joined";

	ss_thr.join();

	LOGD << "recordScreen joined";

	if constexpr (os == OS::Windows) {
		setGDIGamma(brt_slider_steps, 0);
	}
#ifndef _WIN32
	else x11.setInitialGamma(wnd.set_previous_gamma);
#endif

	LOGD << "Exiting";

	return EXIT_SUCCESS;
}

#ifndef _WIN32
void sig_handler(int signo)
{
	LOGD_IF(signo == SIGINT) << "SIGINT received";
	LOGD_IF(signo == SIGTERM) << "SIGTERM received";
	LOGD_IF(signo == SIGQUIT) << "SIGQUIT received";

	save();

	if(!p_quit || ! p_ss_cv || !p_temp_cv) _exit(0);

	*p_quit = true;
	p_ss_cv->notify_one();
	p_temp_cv->notify_one();
}
#endif
