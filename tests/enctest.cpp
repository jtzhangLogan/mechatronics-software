/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/****************************************************************************************
 *
 * This program is used to test the encoders, in particular the velocity and acceleration
 * estimation, assuming that the FPGA/QLA is connected to the FPGA1394-QLA-Test board.
 *
 * Usage: enctest [-pP] <board num>
 *        where P is the Firewire port number (default 0),
 *        or a string such as ethP and fwP, where P is the port number
 *
 *****************************************************************************************/

#ifdef _MSC_VER   // Windows
#define _USE_MATH_DEFINES
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <string>
#include <vector>

#include "PortFactory.h"
#include "AmpIO.h"
#include "Amp1394Time.h"

//**************************************** Approach *******************************************************
//
// The encoder position, p(t), is given by the standard equation:  p(t) = p(0) + v(0)*t + 0.5*a*t*t,
// where p(0) is the initial position, v(0) is the initial velocity and a is the acceleration.
//
// Initially, the position was considered as an angle, theta, with the A and B waveforms given by:
//            A = cos(M_PI*theta/2.0);
//            B = sin(M_PI*theta/2.0);
// The zero crossings of these waveforms can be used to create the encoder transitions.
//
// The current implementation is simpler than this. In particular, we consider that each encoder transition
// corresponds to an increase or decrease in the encoder count. Specifically, for an encoder at p,
// the next transition will be to p+1 or p-1. Thus, if we know that a transition happened at the current
// time, tCur, then we can compute the time of the next transition. Details are given in the ConstantVel
// and ConstantAccel classes.

//*************************************** Motion Class Declaration ****************************************

// Base motion class:
//   Derived classes are MotionInit, ConstantVel, ConstantAccel and Dwell
class MotionBase {
protected:
    double t0;     // initial time
    double tf;     // final time
    double p0;     // initial position
    double pf;     // final position
    double v0;     // initial velocity
    double vf;     // final velocity
public:
    MotionBase(const MotionBase *prevMotion = 0);
    virtual ~MotionBase() {}

    // For invalid motions, the constructor sets tf = t0
    bool IsOK() const { return (tf != t0); }

    // Returns -1.0 when motion is finished
    virtual double CalculateNextTime(double tCur, int &pos, bool &dirChange) = 0;

    void GetInitialValues(double &t, double &p, double &v) const
    { t = t0; p = p0; v = v0; }

    void GetFinalValues(double &t, double &p, double &v) const
    { t = tf; p = pf; v = vf; }
};

// MotionInit: sets starting values for a trajectory
class MotionInit : public MotionBase {
public:
    MotionInit(double vel = 0.0) : MotionBase(0) { tf = 0.0; pf = 0.0; vf = vel; }
    ~MotionInit() {}

    double CalculateNextTime(double /*tCur*/, int &pos, bool &dirChange)
    { pos = 0; dirChange = false; return -1.0; }
};

// ConstantVel:
//    Move at current (non-zero) velocity to desired position
class ConstantVel : public MotionBase {
protected:
    int dir;        // current direction (+1 or -1)
public:
    ConstantVel(double pEnd, bool isInfinite = false, const MotionBase *prevMotion = 0);
    ~ConstantVel() {}

    double CalculateNextTime(double tCur, int &pos, bool &dirChange);
};

// ConstantAccel:
//    Move at current (non-zero) acceleration to desired velocity
class ConstantAccel : public MotionBase {
protected:
    double accel;     // desired acceleration
    double pExtreme;  // extreme position (if direction changes)
    int initDir;      // initial direction (+1 or -1)
    int dir;          // current direction (+1 or -1)
public:
    ConstantAccel(double accel, double vEnd, bool isInfinite = false, const MotionBase *prevMotion = 0);
    ~ConstantAccel() {}

    double CalculateNextTime(double tCur, int &pos, bool &dirChange);
};

// Dwell:
//   Dwell at the current position (zero velocity) for the specified time
class Dwell : public MotionBase {
public:
    Dwell(double deltaT, const MotionBase *prevMotion = 0);
    ~Dwell() {};

    double CalculateNextTime(double /*tCur*/, int &/*pos*/, bool &dirChange)
    { dirChange = false; return -1.0; }
};

// MotionTrajectory:
//   Manages the list of motions
class MotionTrajectory {
    std::vector<MotionBase *> motionList;
    size_t curIndex;    // current index into motionList
    double tCur;        // current time
    int pos;            // current encoder position (counts)

    // Returns last motion in list (0 if list is empty)
    const MotionBase *GetLastMotion(void) const;

public:
    MotionTrajectory() : curIndex(0), tCur(0.0), pos(0) {}
    ~MotionTrajectory() { Init(); }

    // Delete all existing motion segments
    void Init(double vel = 0.0);

    // Add a motion segment
    bool AddConstantVel(double pEnd, bool isInfinite = false);
    bool AddConstantAccel(double accel, double vEnd, bool isInfinite = false);
    bool AddDwell(double deltaT);

    void Restart(void) { tCur = 0.0; curIndex = 0; pos = 0; }
    double GetCurrentTime(void) const { return tCur; }
    double CalculateNextTime(bool &dirChange);

    int  GetEncoderPosition(void) const { return pos; }
    bool GetValuesAtTime(double t, double &p, double &v, double &a) const;
};

//*********************************** Motion Class Methods ****************************************

MotionBase::MotionBase(const MotionBase *prevMotion)
{
    if (prevMotion) {
        t0 = prevMotion->tf;
        p0 = prevMotion->pf;
        v0 = prevMotion->vf;
    }
    else {
        t0 = 0.0;
        p0 = 0.0;
        v0 = 0.0;
    }
}

ConstantVel::ConstantVel(double pEnd, bool isInfinite, const MotionBase *prevMotion) : MotionBase(prevMotion)
{
    vf = v0;
    pf = p0;   // will be updated if not error and not infinite
    tf = t0;   // will be updated if not error
    if (t0 < 0.0) {
        std::cout << "ConstantVel: previous motion is infinite" << std::endl;
    }
    else if (v0 == 0.0) {
        std::cout << "ConstantVel:  zero velocity not allowed (use Dwell instead)" << std::endl;
    }
    else if (isInfinite) {
        // Valid infinite motion
        tf = -1.0;
    }
    else {
        double dt = (pEnd-p0)/v0;
        if (dt <= 0) {
            std::cout << "ConstantVel: invalid motion" << std::endl;
        }
        else {
            // Valid motion
            pf = pEnd;
            tf = t0 + dt;
        }
    }
    dir = (v0 > 0) ? 1 : -1;
}

double ConstantVel::CalculateNextTime(double tCur, int &pos, bool &dirChange)
{
    if ((tf >= 0) && (tCur >= tf)) {
        return -1.0;
    }
    // Next position update:
    // p(t) = p(tCur) + v*(t-tCur))
    //   v > 0, p(t) = p(tCur) + 1 --> v*(t-tCur) = +1 --> t = tCur + 1/v
    //   v < 0, p(t) = p(tCur) - 1 --> v*(t-tCur) = -1 --> t = tCur - 1/v
    // Combining both cases, t = tCur + 1/fabs(v)
    dirChange = false;
    double dt = 1.0/fabs(v0);
    tCur += dt;
    if ((tf < 0) || (tCur <= tf)) {
        pos += dir;
        return tCur;
    }
    else {
        return -1.0;
    }
}

ConstantAccel::ConstantAccel(double acc, double vEnd, bool isInfinite, const MotionBase *prevMotion) : MotionBase(prevMotion), accel(acc)
{
    vf = v0;   // Will be updated if not error and not infinite
    pf = p0;   // Will be updated if not error and not infinite
    tf = t0;   // Will be updated if not error
    if (t0 < 0.0) {
        std::cout << "ConstantAccel: previous motion is infinite" << std::endl;
    }
    else if (accel == 0.0) {
        std::cout << "ConstantAccel:  zero acceleration not allowed" << std::endl;
    }
    else if (isInfinite) {
        // Valid infinite motion
        tf = -1.0;
    }
    else {
        double dt = (vEnd-v0)/accel;
        if (dt <= 0) {
            std::cout << "ConstantAccel: invalid motion" << std::endl;
        }
        else {
            // Valid motion
            vf = vEnd;
            // Final time
            tf = t0 + dt;
            // Final position
            pf = p0 + v0*tf + 0.5*accel*tf*tf;
        }
    }
    if (IsOK()) {
        // Initial direction of motion
        if ((v0 > 0) || ((v0 == 0) && (accel > 0)))
            dir = 1;
        else
            dir = -1;
        // Set initDir (0 means no direction change)
        initDir = (v0*accel < 0) ? dir : 0;
        // Extreme position if there is a direction change (i.e., position when V=0)
        pExtreme = p0 - (v0*v0)/(2.0*accel);
    }
}

double ConstantAccel::CalculateNextTime(double tCur, int &pos, bool &dirChange)
{
    if ((tf >= 0) && (tCur >= tf)) {
        return -1.0;
    }
    double dt = 0.0;
    double vCur = v0 + accel*tCur;
    if ((initDir == 1) && ((pos+dir) > pExtreme)) {
        dirChange = true;
        dir = -1;
        dt = -2.0*vCur/accel;
        std::cout << "Dir change: dt = " << dt << ", init " << initDir << ", pos " << pos << ", dir " << dir << ", e " << pExtreme << std::endl;
    }
    else if ((initDir == -1) && ((pos+dir) < pExtreme)) {
        dirChange = true;
        dir = 1;
        dt = -2.0*vCur/accel;
        std::cout << "Dir change: dt = " << dt << ", init " << initDir << ", pos " << pos << ", dir " << dir << ", e " << pExtreme << std::endl;
    }
    else {
        dirChange = false;
        // Next position update:
        // p(t) = p(tCur) + v(tCur)*(t-tCur) + 1/2*a*(t-tCur)^2, where v(tCur) = v(t0) + a*tCur
        // dir = increment in direction of motion (+1 or -1)
        //   dir = 1: p(t) = p(tCur) + 1 --> v(tCur)*(t-tCur) + 1/2*a*(t-tCur)^2 = +1
        //                --> 1/2*a*(t-tCur)^2 + v(tCur)*(t-tCur) - 1 = 0
        //                --> t = tCur + (-v + sqrt(v^2+2a))/a    (where v = v(tCur))
        //       if a < 0, v^2+2a becomes negative when a < -v^2/2
        //   dir = -1: p(t) = p(tCur) - 1 --> v(tCur)*(t-tCur) + 1/2*a*(t-tCur)^2 = -1
        //                --> 1/2*a*(t-tCur)^2 + v(tCur)*(t-tCur) + 1 = 0
        //                --> t = tCur + (-v + sqrt(v^2-2a))/a    (where v = v(tCur))
        double temp = vCur*vCur+dir*2.0*accel;
        if (temp < 0) {
            std::cout << "Error: negative square root: " << temp << std::endl;
            dt = -vCur/accel;
        }
        else {
            dt = (dir*sqrt(temp)-vCur)/accel;
        }
        if (dt < 0) {
            std::cout << "Error: negative dt: " << dt << std::endl;
            dt = -dt;
        }
    }
    tCur += dt;
    // Update position for next time
    if ((tf < 0) || (tCur <= tf)) {
        pos += dir;
        return tCur;
    }
    else {
        return -1.0;
    }
}

Dwell::Dwell(double deltaT, const MotionBase *prevMotion) : MotionBase(prevMotion)
{
    pf = p0;
    vf = v0;
    tf = t0;   // will be updated if not error
    if (t0 < 0.0) {
        std::cout << "Dwell: previous motion is infinite" << std::endl;
    }
    else if (v0 != 0) {
        std::cout << "Dwell: non-zero velocity not allowed (v0 = " << v0 << ")" << std::endl;
    }
    else {
        tf = t0+deltaT;
    }
}

void MotionTrajectory::Init(double vStart)
{
    for (size_t i = 0; i < motionList.size(); i++)
        delete motionList[i];
    motionList.clear();
    Restart();
    MotionBase *motion = new MotionInit(vStart);
    motionList.push_back(motion);
}

const MotionBase *MotionTrajectory::GetLastMotion(void) const
{
    size_t num = motionList.size();
    return (num > 0) ? motionList[num-1] : 0;
}

bool MotionTrajectory::AddConstantVel(double pEnd, bool isInfinite)
{
    const MotionBase *prevMotion = GetLastMotion();
    MotionBase *motion = new ConstantVel(pEnd, isInfinite, prevMotion);
    if (motion->IsOK())
        motionList.push_back(motion);
    return motion->IsOK();
}

bool MotionTrajectory::AddConstantAccel(double accel, double vEnd, bool isInfinite)
{
    const MotionBase *prevMotion = GetLastMotion();
    MotionBase *motion = new ConstantAccel(accel, vEnd, isInfinite, prevMotion);
    if (motion->IsOK())
        motionList.push_back(motion);
    return motion->IsOK();
}

bool MotionTrajectory::AddDwell(double deltaT)
{
    const MotionBase *prevMotion = GetLastMotion();
    MotionBase *motion = new Dwell(deltaT, prevMotion);
    if (motion->IsOK())
        motionList.push_back(motion);
    return motion->IsOK();
}

double MotionTrajectory::CalculateNextTime(bool &dirChange)
{
    double t = motionList[curIndex]->CalculateNextTime(tCur, pos, dirChange);
    if ((t < 0.0) && (curIndex < motionList.size()-1)) {
        curIndex++;
        t = motionList[curIndex]->CalculateNextTime(tCur, pos, dirChange);
    }
    if (t >= 0.0)
        tCur = t;
    return t;
}

//************************************************************************************************

void TestEncoderVelocity(BasePort *port, AmpIO *board, double vel, double accel)
{
    const int WLEN = 64;
    const int testAxis = 0;   // All axes should be the same when using test board
    quadlet_t waveform[WLEN];
    double dt = board->GetFPGAClockPeriod();
    unsigned int Astate = 1;
    unsigned int Bstate = (vel < 0) ? 0 : 1;
    bool Bnext = false;
    bool dirChange;
    double t = 0.0;
    double lastT = 0.0;
    AmpIO_UInt32 minTicks = 0;  // changed below
    AmpIO_UInt32 maxTicks = 0;
    unsigned int i;

    MotionTrajectory motion;
    motion.Init(vel);
    if (accel == 0)
        motion.AddConstantVel(0.0, true);              // true --> infinite motion
    else
        motion.AddConstantAccel(accel, 0.0, true);     // true --> infinite motion

    for (i = 0; i < WLEN-1; i++) {
        t = motion.CalculateNextTime(dirChange);
        if (dirChange) {
            double theta = vel*t + 0.5*accel*t*t;
            std::cout << "Direction change at t = " << t << ", pos = " << theta << std::endl;
            Bnext = !Bnext;
        }
        if (Bnext)
            Bstate = 1-Bstate;
        else
            Astate = 1-Astate;
        Bnext = !Bnext;
        AmpIO_UInt32 ticks = static_cast<AmpIO_UInt32>((t-lastT)/dt);
        if (i == 0) minTicks = ticks;
        if (ticks < minTicks)
            minTicks = ticks;
        if (ticks > maxTicks)
            maxTicks = ticks;
        lastT = t;
        //std::cout << "waveform[" << i << "]: ticks = " << std::hex << ticks << std::dec
        //          << ", A " << Astate << " B " << Bstate << std::endl;
        waveform[i] = 0x80000000 | (ticks<<8) | (Bstate << 1) | Astate;
    }
    waveform[WLEN-1] = 0;   // Turn off waveform generation

    std::cout << "Created table, total time = " << t << ", tick range: "
              << minTicks << "-" << maxTicks << std::endl;
    if (!board->WriteWaveformTable(waveform, 0, WLEN)) {
        std::cout << "WriteWaveformTable failed" << std::endl;
        return;
    }

    // Initial movements to initialize firmware
    if (vel >= 0) {
         board->WriteDigitalOutput(0x03, 0x02);
         board->WriteDigitalOutput(0x03, 0x00);
         board->WriteDigitalOutput(0x03, 0x01);
         board->WriteDigitalOutput(0x03, 0x03);
    }
    else {
        board->WriteDigitalOutput(0x03, 0x00);
        board->WriteDigitalOutput(0x03, 0x02);
        board->WriteDigitalOutput(0x03, 0x03);
        board->WriteDigitalOutput(0x03, 0x01);
    }
    // Initialize encoder position
    for (i = 0; i < 4; i++)
        board->WriteEncoderPreload(i, 0);
    port->ReadAllBoards();
    std::cout << "Starting position = " << board->GetEncoderPosition(testAxis)
              << ", velocity = " << board->GetEncoderVelocityCountsPerSecond(testAxis)
              << ", acceleration = " << board->GetEncoderAcceleration(testAxis) << std::endl;

    // Start waveform on DOUT1 and DOUT2 (to produce EncA and EncB using test board)
    board->WriteWaveformControl(0x03, 0x03);

    double mpos, mvel, maccel, run;
    AmpIO::EncoderVelocityData encVelData;
    double last_mpos = -1000.0;
    double velSum = 0.0;
    double accelSum = 0.0;
    unsigned int mNum = 0;
    bool waveform_active = true;
    while (waveform_active || (mNum == 0)) {
        port->ReadAllBoards();
        waveform_active = board->GetDigitalInput()&0x20000000;
        if (waveform_active) {
            mpos = board->GetEncoderPosition(testAxis);
            mvel = board->GetEncoderVelocityCountsPerSecond(testAxis);
            maccel = board->GetEncoderAcceleration(testAxis);
            run = board->GetEncoderRunningCounterSeconds(testAxis);
            if (!board->GetEncoderVelocityData(testAxis, encVelData))
                std::cout << "GetEncoderVelocityData failed" << std::endl;
            if ((mpos > 5) || (mpos < -5)) {
                // First few not accurate?
                velSum += mvel;
                accelSum += maccel;
                mNum++;
            }
            bool doEndl = false;
            if (mpos != last_mpos) {
                std::cout << "pos = " << mpos
                          << ", vel = " << mvel
                          << ", accel = " << maccel
                          << ", run = " << run;
                last_mpos = mpos;
                doEndl = true;
            }
            if (encVelData.velOverflow) {doEndl = true; std::cout << " VEL_OVF"; }
            if (encVelData.dirChange)  {doEndl = true; std::cout << " DIR_CHG"; }
            if (encVelData.encError)  {doEndl = true; std::cout << " ENC_ERR"; }
            if (encVelData.qtr1Overflow)  {doEndl = true; std::cout << " Q1_OVF"; }
            if (encVelData.qtr5Overflow)  {doEndl = true; std::cout << " Q5_OVF"; }
            if (encVelData.qtr1Edges!= encVelData.qtr5Edges) {
                doEndl = true;
                std::cout << " EDGES(" << std::hex << static_cast<unsigned int>(encVelData.qtr1Edges)
                          << ", " << static_cast<unsigned int>(encVelData.qtr5Edges) << std::dec << ")";
            }
            if (encVelData.runOverflow)  {doEndl = true; std::cout << " RUN_OVF"; }
            if (doEndl) std::cout << std::endl;
        }
        Amp1394_Sleep(0.0005);
    }
    std::cout << "Average velocity = " << velSum/mNum
              << ", acceleration = " << accelSum/mNum
              << " (" << mNum << " samples)" << std::endl;
}

int main(int argc, char** argv)
{
    int i;
    int board = 0;
    const char *portDescription = "";

    if (argc > 1) {
        int args_found = 0;
        for (i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (argv[i][1] == 'p') {
                    portDescription = argv[i]+2;
                }
                else {
                    std::cerr << "Usage: enctest [<board-num>] [-pP]" << std::endl
                    << "       where <board-num> = rotary switch setting (0-15, default 0)" << std::endl
                    << "             P = port number (default 0)" << std::endl
                    << "                 can also specify -pfwP, -pethP or -pudp" << std::endl;
                    return 0;
                }
            }
            else {
                if (args_found == 0) {
                    board = atoi(argv[i]);
                    std::cerr << "Selecting board " << board << std::endl;
                }
                args_found++;
            }
        }
    }

    BasePort *Port = PortFactory(portDescription);
    if (!Port) {
        std::cerr << "Failed to create port using: " << portDescription << std::endl;
        return -1;
    }
    if (!Port->IsOK()) {
        std::cerr << "Failed to initialize " << Port->GetPortTypeString() << std::endl;
        return -1;
    }
    AmpIO Board(board);
    Port->AddBoard(&Board);

    double vel = 400.0;
    double accel = 0.0;
    char buf[80];
    double temp;
    bool done = false;
    int opt;
    while (!done) {
        std::cout << std::endl
                  << "0) Exit" << std::endl
                  << "1) Set velocity (vel = " << vel << ")" << std::endl
                  << "2) Set acceleration (accel = " << accel << ")" << std::endl
                  << "3) Run test" << std::endl
                  << "Select option: ";

        std::cin.getline(buf, sizeof(buf));
        if (sscanf(buf, "%d", &opt) != 1)
            opt = -1;

        switch (opt) {
            case 0:   // Quit
                done = true;
                std::cout << std::endl;
                break;
            case 1:
                std::cout << "  New velocity: ";
                std::cin.getline(buf, sizeof(buf));
                if (sscanf(buf, "%lf", &temp) == 1)
                    vel = temp;
                else
                    std::cout << "  Invalid velocity: " << buf << std::endl;
                break;
            case 2:
                std::cout << "  New acceleration: ";
                std::cin.getline(buf, sizeof(buf));
                if (sscanf(buf, "%lf", &temp) == 1)
                    accel = temp;
                else
                    std::cout << "  Invalid acceleration: " << buf << std::endl;
                break;
            case 3:
                std::cout << std::endl;
                TestEncoderVelocity(Port, &Board, vel, accel);
                break;
            default:
                std::cout << "  Invalid option!" << std::endl;
        }
    }

    Port->RemoveBoard(board);
    delete Port;
}