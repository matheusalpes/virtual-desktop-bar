#include "VirtualDesktopBar.hpp"

#include <functional>

#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>

#include <KGlobalAccel>

/*
KWindowSystem is needed for moving windows between desktops (as it can list them in stacking order),
and for getting low level information about windows present on desktops. Unfortunately, it doesn't support
changing the number of desktops (see NETRootInfo), let alone removing particular ones (see LibTaskManager).
Also, renaming desktops with its setDesktopName method isn't reliable (old names appear after rebooting).

NETRootInfo provides a method for setting the number of desktops, but brings additional X11 dependencies.

LibTaskManager provides a method for removing particular desktops, and for adding desktops at given positions,
but these work only on Wayland. On X11 they just remove the last desktop, and append a new one after the last one.
Also, LibTaskManager doesn't seem to provide any method for renaming desktops at all. That's a bummer.

KWin's D-Bus service seems to be the most comprehensive. It provides methods for removing particular desktops,
adding desktops at given positions, even on X11, and also provides a method for renaming desktops.
*/

VirtualDesktopBar::VirtualDesktopBar(QObject* parent) : QObject(parent),
        dbusInterface("org.kde.KWin", "/VirtualDesktopManager"),
        dbusInterfaceName("org.kde.KWin.VirtualDesktopManager"),
        sendDesktopInfoListLock(false),
        tryAddEmptyDesktopLock(false),
        tryRemoveEmptyDesktopsLock(false),
        tryRenameEmptyDesktopsLock(false),
        currentDesktopNumber(KWindowSystem::currentDesktop()),
        mostRecentDesktopNumber(currentDesktopNumber) {

    setUpSignals();
    setUpGlobalKeyboardShortcuts();
    setUpWindowNameSubstitutions();
}

void VirtualDesktopBar::requestDesktopInfoList() {
    sendDesktopInfoList();
}

void VirtualDesktopBar::showDesktop(int number) {
    KWindowSystem::setCurrentDesktop(number);
}

void VirtualDesktopBar::addDesktop(unsigned position) {
    if (position == 0) {
        position = KWindowSystem::numberOfDesktops() + 1;
    }

    QString name = "New Desktop";
    dbusInterface.call("createDesktop", position, name);

    if (!cfg_AddingDesktopsExecuteCommand.isEmpty()) {
        QTimer::singleShot(100, [=] {
            QString command = "(" + cfg_AddingDesktopsExecuteCommand + ") & disown";
            system(command.toStdString().c_str());
        });
    }
}

void VirtualDesktopBar::removeDesktop(int number) {
    dbusInterface.call("removeDesktop", getDesktopInfo(number).id);
}

void VirtualDesktopBar::renameDesktop(int number, QString name) {
    dbusInterface.call("setDesktopName", getDesktopInfo(number).id, name);
}

void VirtualDesktopBar::replaceDesktops(int number1, int number2) {
    if (number1 == number2) {
        return;
    }
    if (number1 < 1 || number1 > KWindowSystem::numberOfDesktops()) {
        return;
    }
    if (number2 < 1 || number2 > KWindowSystem::numberOfDesktops()) {
        return;
    }

    auto desktopInfo1 = getDesktopInfo(number1);
    auto desktopInfo2 = getDesktopInfo(number2);

    auto windowInfoList1 = getWindowInfoList(desktopInfo1.number);
    auto windowInfoList2 = getWindowInfoList(desktopInfo2.number);

    if (desktopInfo1.isCurrent) {
        KWindowSystem::setCurrentDesktop(desktopInfo2.number);
    } else if (desktopInfo2.isCurrent) {
        KWindowSystem::setCurrentDesktop(desktopInfo1.number);
    }

    for (auto& windowInfo : windowInfoList2) {
        KWindowSystem::setOnDesktop(windowInfo.win(), desktopInfo1.number);
    }

    for (auto& windowInfo : windowInfoList1) {
        KWindowSystem::setOnDesktop(windowInfo.win(), desktopInfo2.number);
    }

    renameDesktop(desktopInfo1.number, desktopInfo2.name);
    renameDesktop(desktopInfo2.number, desktopInfo1.name);
}

void VirtualDesktopBar::cfg_EmptyDesktopsRenameAsChanged() {
    tryRenameEmptyDesktops();
}

void VirtualDesktopBar::cfg_AddingDesktopsExecuteCommandChanged() {
}

void VirtualDesktopBar::cfg_DynamicDesktopsEnableChanged() {
    tryAddEmptyDesktop();
    tryRemoveEmptyDesktops();
}

void VirtualDesktopBar::cfg_MultipleScreensFilterOccupiedDesktopsChanged() {
    sendDesktopInfoList();
}

void VirtualDesktopBar::setUpSignals() {
    auto s = KWindowSystem::self();
    auto r = this;

    QObject::connect(s, &KWindowSystem::currentDesktopChanged, r, [&] {
        updateLocalDesktopNumbers();
        processChanges([&] { sendDesktopInfoList(); }, sendDesktopInfoListLock);
    });

    QObject::connect(s, &KWindowSystem::numberOfDesktopsChanged, r, [&] {
        processChanges([&] { tryAddEmptyDesktop(); }, tryAddEmptyDesktopLock);
        processChanges([&] { tryRemoveEmptyDesktops(); }, tryRemoveEmptyDesktopsLock);
        processChanges([&] { tryRenameEmptyDesktops(); }, tryRenameEmptyDesktopsLock);
        processChanges([&] { sendDesktopInfoList(); }, sendDesktopInfoListLock);
    });

    QObject::connect(s, &KWindowSystem::desktopNamesChanged, r, [&] {
        processChanges([&] { sendDesktopInfoList(); }, sendDesktopInfoListLock);
    });

    QObject::connect(s, static_cast<void (KWindowSystem::*)(WId, NET::Properties, NET::Properties2)>
                        (&KWindowSystem::windowChanged), r, [&](WId, NET::Properties properties, NET::Properties2) {
        if (properties & NET::WMState) {
            processChanges([&] { tryAddEmptyDesktop(); }, tryAddEmptyDesktopLock);
            processChanges([&] { tryRemoveEmptyDesktops(); }, tryRemoveEmptyDesktopsLock);
            processChanges([&] { tryRenameEmptyDesktops(); }, tryRenameEmptyDesktopsLock);
            processChanges([&] { sendDesktopInfoList(); }, sendDesktopInfoListLock);
        }
    });
}

void VirtualDesktopBar::setUpGlobalKeyboardShortcuts() {
    QString prefix = "Virtual Desktop Bar - ";
    actionCollection = new KActionCollection(this, QStringLiteral("kwin"));

    actionSwitchToRecentDesktop = actionCollection->addAction(QStringLiteral("switchToRecentDesktop"));
    actionSwitchToRecentDesktop->setText(prefix + "Switch to Recent Desktop");
    QObject::connect(actionSwitchToRecentDesktop, &QAction::triggered, this, [&] {
        showDesktop(mostRecentDesktopNumber);
    });
    KGlobalAccel::setGlobalShortcut(actionSwitchToRecentDesktop, QKeySequence());

    actionAddDesktop = actionCollection->addAction(QStringLiteral("addDesktop"));
    actionAddDesktop->setText(prefix + "Add Desktop");
    QObject::connect(actionAddDesktop, &QAction::triggered, this, [&] {
        addDesktop();
    });
    KGlobalAccel::setGlobalShortcut(actionAddDesktop, QKeySequence());

    actionRemoveLastDesktop = actionCollection->addAction(QStringLiteral("removeLastDesktop"));
    actionRemoveLastDesktop->setText(prefix + "Remove Last Desktop");
    QObject::connect(actionRemoveLastDesktop, &QAction::triggered, this, [&] {
        removeDesktop(KWindowSystem::numberOfDesktops());
    });
    KGlobalAccel::setGlobalShortcut(actionRemoveLastDesktop, QKeySequence());

    actionRemoveCurrentDesktop = actionCollection->addAction(QStringLiteral("removeCurrentDesktop"));
    actionRemoveCurrentDesktop->setText(prefix + "Remove Current Desktop");
    QObject::connect(actionRemoveCurrentDesktop, &QAction::triggered, this, [&] {
        removeDesktop(KWindowSystem::currentDesktop());
    });
    KGlobalAccel::setGlobalShortcut(actionRemoveCurrentDesktop, QKeySequence());

    actionRenameCurrentDesktop = actionCollection->addAction(QStringLiteral("renameCurrentDesktop"));
    actionRenameCurrentDesktop->setText(prefix + "Rename Current Desktop");
    QObject::connect(actionRenameCurrentDesktop, &QAction::triggered, this, [&] {
        emit requestRenameCurrentDesktop();
    });
    KGlobalAccel::setGlobalShortcut(actionRenameCurrentDesktop, QKeySequence());

    actionMoveCurrentDesktopToLeft = actionCollection->addAction(QStringLiteral("moveCurrentDesktopToLeft"));
    actionMoveCurrentDesktopToLeft->setText("Move Current Desktop to Left");
    QObject::connect(actionMoveCurrentDesktopToLeft, &QAction::triggered, this, [&] {
        replaceDesktops(KWindowSystem::currentDesktop(),
                        KWindowSystem::currentDesktop() - 1);
    });
    KGlobalAccel::setGlobalShortcut(actionMoveCurrentDesktopToLeft, QKeySequence());

    actionMoveCurrentDesktopToRight = actionCollection->addAction(QStringLiteral("moveCurrentDesktopToRight"));
    actionMoveCurrentDesktopToRight->setText("Move Current Desktop to Right");
    QObject::connect(actionMoveCurrentDesktopToRight, &QAction::triggered, this, [&] {
        replaceDesktops(KWindowSystem::currentDesktop(),
                        KWindowSystem::currentDesktop() + 1);
    });
    KGlobalAccel::setGlobalShortcut(actionMoveCurrentDesktopToRight, QKeySequence());
}

void VirtualDesktopBar::setUpWindowNameSubstitutions() {
    windowNameSubstitutionMap.insert("Gimp-*.", "GIMP");
    windowNameSubstitutionMap.insert("dolphin", "Dolphin");
    windowNameSubstitutionMap.insert("kate", "Kate");
    windowNameSubstitutionMap.insert("konsole", "Konsole");
    windowNameSubstitutionMap.insert("ksysguard", "KSysGuard");
    windowNameSubstitutionMap.insert("lattedock", "Latte Dock");
    windowNameSubstitutionMap.insert("libreoffice-*", "LibreOffice");
    windowNameSubstitutionMap.insert("okular", "Okular");
    windowNameSubstitutionMap.insert("systemsettings", "Settings");
}

void VirtualDesktopBar::processChanges(std::function<void()> callback, bool& lock) {
    if (!lock) {
        lock = true;
        QTimer::singleShot(1, [this, callback, &lock] {
            lock = false;
            callback();
        });
    }
}

DesktopInfo VirtualDesktopBar::getDesktopInfo(int number) {
    for (auto& desktopInfo : getDesktopInfoList()) {
        if (desktopInfo.number == number) {
            return desktopInfo;
        }
    }
    return DesktopInfo();
}

DesktopInfo VirtualDesktopBar::getDesktopInfo(QString id) {
    for (auto& desktopInfo : getDesktopInfoList()) {
        if (desktopInfo.id == id) {
            return desktopInfo;
        }
    }
    return DesktopInfo();
}

QList<DesktopInfo> VirtualDesktopBar::getDesktopInfoList(bool extraInfo) {
    QList<DesktopInfo> desktopInfoList;

    // Getting info about desktops through the KWin's D-Bus service here
    auto reply = dbusInterface.call("Get", dbusInterfaceName, "desktops");

    // Extracting data from the D-Bus reply message here
    // More details at https://stackoverflow.com/a/20206377
    auto something = reply.arguments().at(0).value<QDBusVariant>();
    auto somethingSomething = something.variant().value<QDBusArgument>();
    somethingSomething >> desktopInfoList;

    for (auto& desktopInfo : desktopInfoList) {
        desktopInfo.isCurrent = desktopInfo.number == KWindowSystem::currentDesktop();

        if (!extraInfo) {
            continue;
        }

        auto windowInfoList = getWindowInfoList(desktopInfo.number);

        desktopInfo.isEmpty = windowInfoList.isEmpty();

        for (int i = 0; i < windowInfoList.length(); i++) {
            auto& windowInfo = windowInfoList[i];

            if (!desktopInfo.isUrgent) {
                desktopInfo.isUrgent = windowInfo.hasState(NET::DemandsAttention);
            }

            QString windowName = windowInfo.windowClassClass();

            auto const mapKeys = windowNameSubstitutionMap.keys();
            for (auto& pattern : mapKeys) {
                QRegularExpression regex(pattern);
                if (regex.match(windowName).hasMatch()) {
                    windowName = windowNameSubstitutionMap.value(pattern);
                    break;
                }
            }

            if (i == 0) {
                desktopInfo.activeWindowName = windowName;
            }

            desktopInfo.windowNameList << windowName;
        }
    }

    return desktopInfoList;
}

QList<KWindowInfo> VirtualDesktopBar::getWindowInfoList(int desktopNumber, bool ignoreScreens) {
    QList<KWindowInfo> windowInfoList;

    auto screenRect = QGuiApplication::screens().at(0)->geometry();

    QList<WId> windowIds = KWindowSystem::stackingOrder();
    for (int i = windowIds.length() - 1; i >= 0; i--) {
        KWindowInfo windowInfo(windowIds[i], NET::WMState |
                                             NET::WMDesktop |
                                             NET::WMGeometry |
                                             NET::WMWindowType,
                                             NET::WM2WindowClass);

        // Skipping windows not present on the current desktops
        if (windowInfo.desktop() != desktopNumber &&
            windowInfo.desktop() != -1) {
            continue;
        }

        // Skipping windows if they wish so
        if (windowInfo.hasState(NET::SkipPager) ||
            windowInfo.hasState(NET::SkipTaskbar) ||
            windowInfo.windowType(NET::NormalMask)) {
            continue;
        }

        // Skipping windows not present on the current screen
        if (!ignoreScreens && cfg_MultipleScreensFilterOccupiedDesktops) {
            auto windowRect = windowInfo.geometry();
            auto intersectionRect = screenRect.intersected(windowRect);
            if (intersectionRect.width() < windowRect.width() / 2 ||
                intersectionRect.height() < windowRect.height() / 2) {
                continue;
            }
        }

        windowInfoList << windowInfo;
    }

    return windowInfoList;
}

QList<int> VirtualDesktopBar::getEmptyDesktopNumberList(bool noCheating) {
    QList<int> emptyDesktopNumberList;

    for (int i = 1; i <= KWindowSystem::numberOfDesktops(); i++) {
        auto windowInfoList = getWindowInfoList(i, true);

        if (noCheating) {
            if (windowInfoList.empty()) {
                emptyDesktopNumberList << i;
            }
            continue;
        }

        bool isConsideredEmpty = true;
        for (auto& windowInfo : windowInfoList) {
            if (windowInfo.desktop() == i) {
                isConsideredEmpty = false;
                break;
            }
        }

        if (isConsideredEmpty) {
            emptyDesktopNumberList << i;
        }
    }

    return emptyDesktopNumberList;
}

void VirtualDesktopBar::sendDesktopInfoList() {
    QVariantList desktopInfoList;
    for (auto& desktopInfo : getDesktopInfoList(true)) {
        desktopInfoList << desktopInfo.toQVariantMap();
    }
    emit desktopInfoListSent(desktopInfoList);
}
void VirtualDesktopBar::tryAddEmptyDesktop() {
    if (cfg_DynamicDesktopsEnable) {
        auto emptyDesktopNumberList = getEmptyDesktopNumberList(false);
        if (emptyDesktopNumberList.empty()) {
            addDesktop();
        }
    }
}

void VirtualDesktopBar::tryRemoveEmptyDesktops() {
    if (cfg_DynamicDesktopsEnable) {
        auto emptyDesktopNumberList = getEmptyDesktopNumberList(false);
        for (int i = 1; i < emptyDesktopNumberList.length(); i++) {
            int desktopNumber = emptyDesktopNumberList[i];
            removeDesktop(desktopNumber);
        }
    }
}

void VirtualDesktopBar::tryRenameEmptyDesktops() {
    if (!cfg_EmptyDesktopsRenameAs.isEmpty()) {
        auto emptyDesktopNumberList = getEmptyDesktopNumberList();
        for (int desktopNumber : emptyDesktopNumberList) {
            renameDesktop(desktopNumber, cfg_EmptyDesktopsRenameAs);
        }
    }
}

void VirtualDesktopBar::updateLocalDesktopNumbers() {
    int n = KWindowSystem::currentDesktop();
    if (currentDesktopNumber != n) {
        mostRecentDesktopNumber = currentDesktopNumber;
    }
    currentDesktopNumber = n;
}
