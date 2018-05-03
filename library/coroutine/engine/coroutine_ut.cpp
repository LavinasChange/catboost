#include "impl.h"

#include <library/unittest/registar.h>

#include <util/string/cast.h>
#include <util/system/pipe.h>

class TCoroTest: public TTestBase {
    UNIT_TEST_SUITE(TCoroTest);
    UNIT_TEST(TestSimpleX1);
    UNIT_TEST(TestSimpleX2);
    UNIT_TEST(TestSimpleX3);
    UNIT_TEST(TestMemFun);
    UNIT_TEST(TestMutex);
    UNIT_TEST(TestCondVar);
    UNIT_TEST(TestJoin);
    UNIT_TEST(TestException);
    UNIT_TEST(TestJoinCancelExitRaceBug);
    UNIT_TEST(TestWaitWakeLivelockBug);
    UNIT_TEST(TestFastPathWake)
    UNIT_TEST_SUITE_END();

public:
    void TestException();
    void TestSimpleX1();
    void TestSimpleX2();
    void TestSimpleX3();
    void TestMemFun();
    void TestMutex();
    void TestCondVar();
    void TestJoin();
    void TestJoinCancelExitRaceBug();
    void TestWaitWakeLivelockBug();
    void TestFastPathWake();
};

void TCoroTest::TestException() {
    TContExecutor e(1000000);

    bool f2run = false;

    auto f1 = [&f2run](TCont* c) {
        struct TCtx {
            ~TCtx() {
                Y_VERIFY(!*F2);

                C->Yield();
            }

            TCont* C;
            bool* F2;
        };

        try {
            TCtx ctx = {c, &f2run};

            throw 1;
        } catch (...) {
        }
    };

    bool unc = true;

    auto f2 = [&unc, &f2run](TCont*) {
        f2run = true;
        unc = std::uncaught_exception();

        // check segfault
        try {
            throw 2;
        } catch (int) {
        }
    };

    e.Create(f1, "f1");
    e.Create(f2, "f2");
    e.Execute();

    UNIT_ASSERT(!unc);
}

static int i0;

static void CoRun(TCont* c, void* /*run*/) {
    while (i0 < 100000) {
        ++i0;
        c->Yield();
    }
}

static void CoMain(TCont* c, void* /*arg*/) {
    for (volatile size_t i2 = 0; i2 < 10; ++i2) {
        c->Executor()->Create(CoRun, nullptr, "run");
    }
}

void TCoroTest::TestSimpleX1() {
    i0 = 0;
    TContExecutor e(32000);
    e.Execute(CoMain);
    UNIT_ASSERT_VALUES_EQUAL(i0, 100000);
}

struct TTestObject {
    int i = 0;
    int j = 0;

public:
    void RunTask1(TCont*) {
        i = 1;
    }
    void RunTask2(TCont*) {
        j = 2;
    }
};

void TCoroTest::TestMemFun() {
    i0 = 0;
    TContExecutor e(32000);
    TTestObject obj;
    e.Create<TTestObject, &TTestObject::RunTask1>(&obj, "test1");
    e.Execute<TTestObject, &TTestObject::RunTask2>(&obj);
    UNIT_ASSERT_EQUAL(obj.i, 1);
    UNIT_ASSERT_EQUAL(obj.j, 2);
}

void TCoroTest::TestSimpleX2() {
    {
        i0 = 0;

        {
            TContExecutor e(32000);
            e.Execute(CoMain);
        }

        UNIT_ASSERT_EQUAL(i0, 100000);
    }

    {
        i0 = 0;

        {
            TContExecutor e(32000);
            e.Execute(CoMain);
        }

        UNIT_ASSERT_EQUAL(i0, 100000);
    }
}

struct TRunner {
    inline TRunner()
        : Runs(0)
    {
    }

    inline void operator()(TCont* c) {
        ++Runs;
        c->Yield();
    }

    size_t Runs;
};

void TCoroTest::TestSimpleX3() {
    TContExecutor e(32000);
    TRunner runner;

    for (volatile size_t i3 = 0; i3 < 1000; ++i3) {
        e.Create(runner, "runner");
    }

    e.Execute();

    UNIT_ASSERT_EQUAL(runner.Runs, 1000);
}

static TString res;
static TContMutex mutex;

static void CoMutex(TCont* c, void* /*run*/) {
    {
        mutex.LockI(c);
        c->Yield();
        res += c->Name();
        mutex.UnLock();
    }

    c->Yield();

    {
        mutex.LockI(c);
        c->Yield();
        res += c->Name();
        mutex.UnLock();
    }
}

static void CoMutexTest(TCont* c, void* /*run*/) {
    c->Executor()->Create(CoMutex, nullptr, "1");
    c->Executor()->Create(CoMutex, nullptr, "2");
}

void TCoroTest::TestMutex() {
    TContExecutor e(32000);
    e.Execute(CoMutexTest);
    UNIT_ASSERT_EQUAL(res, "1212");
    res.clear();
}

static TContMutex m1;
static TContCondVar c1;

static void CoCondVar(TCont* c, void* /*run*/) {
    for (size_t i4 = 0; i4 < 3; ++i4) {
        UNIT_ASSERT_EQUAL(m1.LockI(c), 0);
        UNIT_ASSERT_EQUAL(c1.WaitI(c, &m1), 0);
        res += c->Name();
        m1.UnLock();
    }
}

static void CoCondVarTest(TCont* c, void* /*run*/) {
    c->Executor()->Create(CoCondVar, nullptr, "1");
    c->Yield();
    c->Executor()->Create(CoCondVar, nullptr, "2");
    c->Yield();
    c->Executor()->Create(CoCondVar, nullptr, "3");
    c->Yield();
    c->Executor()->Create(CoCondVar, nullptr, "4");
    c->Yield();
    c->Executor()->Create(CoCondVar, nullptr, "5");
    c->Yield();
    c->Executor()->Create(CoCondVar, nullptr, "6");
    c->Yield();

    for (size_t i5 = 0; i5 < 3; ++i5) {
        res += ToString((size_t)i5) + "^";
        c1.BroadCast();
        c->Yield();
    }
}

void TCoroTest::TestCondVar() {
    TContExecutor e(32000);
    e.Execute(CoCondVarTest);
    UNIT_ASSERT_EQUAL(res, "0^1234561^1234562^123456");
    res.clear();
}

void TCoroTest::TestJoin() {
    struct TSleepCont {
        const TInstant Deadline;
        int Result;

        inline void operator()(TCont* c) {
            Result = c->SleepD(Deadline);
        }
    };

    struct TReadCont {
        const TInstant Deadline;
        const SOCKET Sock;
        int Result;

        inline void operator()(TCont* c) {
            char buf = 0;
            Result = c->ReadD(Sock, &buf, sizeof(buf), Deadline).Status();
        }
    };

    struct TJoinCont {
        const TInstant Deadline;
        TCont* const Cont;
        bool Result;

        inline void operator()(TCont* c) {
            Result = c->Join(Cont, Deadline);
        }
    };

    TContExecutor e(32000);

    TPipe in, out;
    TPipe::Pipe(in, out);

    SetNonBlock(in.GetHandle());

    {
        TSleepCont sc = {TInstant::Max(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(100).ToDeadLine(), e.Create(sc, "sc")->ContPtr(), true};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(sc.Result, ECANCELED);
        UNIT_ASSERT_EQUAL(jc.Result, false);
    }

    {
        TSleepCont sc = {TDuration::MilliSeconds(100).ToDeadLine(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(200).ToDeadLine(), e.Create(sc, "sc")->ContPtr(), false};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(sc.Result, ETIMEDOUT);
        UNIT_ASSERT_EQUAL(jc.Result, true);
    }

    {
        TSleepCont sc = {TDuration::MilliSeconds(200).ToDeadLine(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(100).ToDeadLine(), e.Create(sc, "sc")->ContPtr(), true};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(sc.Result, ECANCELED);
        UNIT_ASSERT_EQUAL(jc.Result, false);
    }

    {
        TReadCont rc = {TInstant::Max(), in.GetHandle(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(100).ToDeadLine(), e.Create(rc, "rc")->ContPtr(), true};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(rc.Result, ECANCELED);
        UNIT_ASSERT_EQUAL(jc.Result, false);
    }

    {
        TReadCont rc = {TDuration::MilliSeconds(100).ToDeadLine(), in.GetHandle(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(200).ToDeadLine(), e.Create(rc, "rc")->ContPtr(), false};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(rc.Result, ETIMEDOUT);
        UNIT_ASSERT_EQUAL(jc.Result, true);
    }

    {
        TReadCont rc = {TDuration::MilliSeconds(200).ToDeadLine(), in.GetHandle(), 0};
        TJoinCont jc = {TDuration::MilliSeconds(100).ToDeadLine(), e.Create(rc, "rc")->ContPtr(), true};

        e.Execute(jc);

        UNIT_ASSERT_EQUAL(rc.Result, ECANCELED);
        UNIT_ASSERT_EQUAL(jc.Result, false);
    }
}

namespace NCoroJoinCancelExitRaceBug {
    struct TState {
        TCont* Sub = nullptr;
    };

    static void DoAux(TCont*, void* argPtr) noexcept {
        TState& state = *(TState*)(argPtr);

        // 06.{Ready:[Sub2]} > {Ready:[Sub2,Sub]}
        state.Sub->Cancel();
    }

    static void DoSub2(TCont*, void*) noexcept {
        // 07.{Ready:[Sub]} > Exit > {Ready:[Sub],ToDelete:[Sub2]}
        // 08.{Ready:[Sub],ToDelete:[Sub2]} > Release(Sub2) > {Ready:[Sub],Deleted:[Sub2]}
    }

    static void DoSub(TCont* cont, void* argPtr) noexcept {
        TState& state = *(TState*)(argPtr);
        state.Sub = cont;

        // 04.{Ready:[Aux]} > {Ready:[Aux,Sub2]}
        auto* sub2 = cont->Executor()->Create(DoSub2, argPtr, "Sub2");

        // 05.{Ready:[Aux,Sub2]} > SwitchTo(Aux)
        // 09.{Ready:[],Deleted:[Sub2]} > Cancel(Sub2) > {Ready:[Sub2],Deleted:[Sub2]}
        // 10.{Ready:[Sub2],Deleted:[Sub2]} > SwitchTo(Sub2) > FAIL: can not return from exit
        cont->Join(sub2->ContPtr());

        state.Sub = nullptr;
    }

    static void DoMain(TCont* cont) noexcept {
        TState state;

        // 01.{Ready:[]} > {Ready:[Sub]}
        auto* sub = cont->Executor()->Create(DoSub, &state, "Sub");

        // 02.{Ready:[Sub]} > {Ready:[Sub,Aux]}
        cont->Executor()->Create(DoAux, &state, "Aux");

        // 03.{Ready:[Sub,Aux]} > SwitchTo(Sub)
        cont->Join(sub->ContPtr());
    }
}

void TCoroTest::TestJoinCancelExitRaceBug() {
    TContExecutor exec(20000);
    exec.SetFailOnError(true);
    exec.Execute(NCoroJoinCancelExitRaceBug::DoMain);
}

namespace NCoroWaitWakeLivelockBug {
    struct TState;

    struct TSubState {
        TSubState(TState& parent, ui32 self)
            : Parent(parent)
            , Name(TStringBuilder() << "Sub" << self)
            , Self(self)
        {
            UNIT_ASSERT(self < 2);
        }

        TSubState& OtherState();

        TState& Parent;
        TTimerEvent* Event = nullptr;
        TCont* Cont = nullptr;
        TString Name;
        ui32 Self = -1;
    };

    struct TState {
        TState()
            : Subs{{*this, 0}, {*this, 1}}
        {}

        TSubState Subs[2];
        bool Stop = false;
    };

    TSubState& TSubState::OtherState() {
        return Parent.Subs[1 - Self];
    }

    static void DoStop(TCont* cont, void* argPtr) {
        TState& state = *(TState*)(argPtr);

        TTimerEvent event(cont, TInstant::Now());
        ExecuteEvent(&event);
        state.Stop = true;
        for (auto& sub: state.Subs) {
            if (sub.Event) {
                sub.Event->Wake(EWAKEDUP);
            }
        }
    }

    static void DoSub(TCont* cont, void* argPtr) {
        TSubState& state = *(TSubState*)(argPtr);

        while (!state.Parent.Stop) {
            TTimerEvent event(cont, TInstant::Max());
            if (state.OtherState().Event) {
                state.OtherState().Event->Wake(EWAKEDUP);
            }
            state.Event = &event;
            ExecuteEvent(&event);
            state.Event = nullptr;
        }

        state.Cont = nullptr;
    }

    static void DoMain(TCont* cont) noexcept {
        TState state;

        for (auto& subState : state.Subs) {
            subState.Cont = cont->Executor()->Create(DoSub, &subState, ~subState.Name)->ContPtr();
        }

        cont->Join(
            cont->Executor()->Create(DoStop, &state, "Stop")->ContPtr()
        );

        for (auto& subState : state.Subs) {
            if (subState.Cont) {
                cont->Join(subState.Cont);
            }
        }
    }
}

void TCoroTest::TestWaitWakeLivelockBug() {
    TContExecutor exec(20000);
    exec.SetFailOnError(true);
    exec.Execute(NCoroWaitWakeLivelockBug::DoMain);
}

namespace NCoroTestFastPathWake {
    struct TState;

    struct TSubState {
        TSubState(TState& parent, ui32 self)
            : Parent(parent)
            , Name(TStringBuilder() << "Sub" << self)
        {}

        TState& Parent;
        TInstant Finish;
        TTimerEvent* Event = nullptr;
        TCont* Cont = nullptr;
        TString Name;
    };

    struct TState {
        TState()
            : Subs{{*this, 0}, {*this, 1}}
        {
            TPipe::Pipe(In, Out);
            SetNonBlock(In.GetHandle());
        }

        TSubState Subs[2];
        TPipe In, Out;
        bool IoSleepRunning = false;
    };

    static void DoIoSleep(TCont* cont, void* argPtr) noexcept {
        try {
            TState& state = *(TState*) (argPtr);
            state.IoSleepRunning = true;

            TTempBuf tmp;
            // Wait for the event from io
            auto res = cont->ReadD(state.In.GetHandle(), tmp.Data(), 1, TDuration::Seconds(10).ToDeadLine());
            // TODO (velavokr): BALANCER-1338 Bug: Read does not react on Close.
            UNIT_ASSERT_VALUES_EQUAL(res.Checked(), 0);
//        UNIT_ASSERT_VALUES_EQUAL(res.Checked(), 1);
            state.IoSleepRunning = false;
        } catch (const NUnitTest::TAssertException& ex) {
            Cerr << ex.AsStrBuf() << Endl;
            ex.BackTrace()->PrintTo(Cerr);
            throw;
        } catch (...) {
            Cerr << CurrentExceptionMessage() << Endl;
            throw;
        }
    }

    static void DoSub(TCont* cont, void* argPtr) noexcept {
        TSubState& state = *(TSubState*)(argPtr);

        TTimerEvent event(cont, TInstant::Max());
        state.Event = &event;
        ExecuteEvent(&event);
        state.Event = nullptr;
        state.Cont = nullptr;
        state.Finish = TInstant::Now();
    }

    static void DoMain(TCont* cont) noexcept {
        try {
            TState state;
            TInstant start = TInstant::Now();

            // This guy sleeps on io
            auto sleeper = cont->Executor()->Create(DoIoSleep, &state, "io_sleeper");

            // These guys are to be woken up right away
            for (auto& subState : state.Subs) {
                subState.Cont = cont->Executor()->Create(DoSub, &subState, ~subState.Name)->ContPtr();
            }

            // Give way
            cont->Yield();

            // Check everyone has started, wake those to be woken
            UNIT_ASSERT(state.IoSleepRunning);

            for (auto& subState : state.Subs) {
                UNIT_ASSERT(subState.Event);
                subState.Event->Wake(EWAKEDUP);
            }

            // Give way again
            cont->Yield();

            // Check the woken guys have finished and quite soon
            for (auto& subState : state.Subs) {
                UNIT_ASSERT(subState.Finish - start < TDuration::MilliSeconds(100));
                UNIT_ASSERT(!subState.Cont);
            }

            // Wake the io guy and finish
//        state.Out.Write("1", 1);
            // TODO (velavokr): BALANCER-1338 Bug: Read does not react on Close.
            state.Out.Close();

            if (state.IoSleepRunning) {
                cont->Join(sleeper->ContPtr());
            }

            // Check everything has ended sooner than the timeout
            UNIT_ASSERT(TInstant::Now() - start < TDuration::Seconds(1));
        } catch (const NUnitTest::TAssertException& ex) {
            Cerr << ex.AsStrBuf() << Endl;
            ex.BackTrace()->PrintTo(Cerr);
            throw;
        } catch (...) {
            Cerr << CurrentExceptionMessage() << Endl;
            throw;
        }
    }
}

void TCoroTest::TestFastPathWake() {
    TContExecutor exec(20000);
    exec.SetFailOnError(true);
    exec.Execute(NCoroTestFastPathWake::DoMain);
}

UNIT_TEST_SUITE_REGISTRATION(TCoroTest);
