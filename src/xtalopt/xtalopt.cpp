/**********************************************************************
  XtalOpt - "Engine" for the optimization process

  Copyright (C) 2009-2010 by David C. Lonie

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 ***********************************************************************/

// Prevent redefinition of symbols on windows
#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#endif

#include <xtalopt/xtalopt.h>

#include <xtalopt/structures/xtal.h>
#include <xtalopt/optimizers/vasp.h>
#include <xtalopt/optimizers/gulp.h>
#include <xtalopt/optimizers/pwscf.h>
#include <xtalopt/optimizers/castep.h>
#include <xtalopt/ui/dialog.h>
#include <xtalopt/genetic.h>

#include <globalsearch/optbase.h>
#include <globalsearch/optimizer.h>
#include <globalsearch/queuemanager.h>
#include <globalsearch/sshmanager.h>
#include <globalsearch/macros.h>
#include <globalsearch/bt.h>

#include <QtCore/QDir>
#include <QtCore/QList>
#include <QtCore/QFile>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QReadWriteLock>
#include <QtCore/QtConcurrentRun>

#include <QtGui/QMessageBox>

#define ANGSTROM_TO_BOHR 1.889725989

using namespace GlobalSearch;
using namespace OpenBabel;
using namespace Avogadro;

namespace XtalOpt {

  XtalOpt::XtalOpt(XtalOptDialog *parent) :
    OptBase(parent)
  {
    xtalInitMutex = new QMutex;
    m_idString = "XtalOpt";

    // Connections
    connect(m_tracker, SIGNAL(newStructureAdded(GlobalSearch::Structure*)),
            this, SLOT(checkForDuplicates()));
    connect(this, SIGNAL(sessionStarted()),
            this, SLOT(resetDuplicates()));
  }

  XtalOpt::~XtalOpt()
  {
  }

  void XtalOpt::startSearch() {
    debug("Starting optimization.");
    emit startingSession();

    // Settings checks
    // Check lattice parameters, volume, etc
    if (!XtalOpt::checkLimits()) {
      error("Cannot create structures. Check log for details.");
      return;
    }

    // Do we have a composition?
    if (comp.isEmpty()) {
      error("Cannot create structures. Composition is not set.");
      return;
    }

    // VASP checks:
    if (m_optimizer->getIDString() == "VASP") {
      // Is the POTCAR generated? If not, warn user in log and launch generator.
      // Every POTCAR will be identical in this case!
      QList<uint> oldcomp, atomicNums = comp.keys();
      QList<QVariant> oldcomp_ = m_optimizer->getData("Composition").toList();
      for (int i = 0; i < oldcomp_.size(); i++)
        oldcomp.append(oldcomp_.at(i).toUInt());
      qSort(atomicNums);
      if (m_optimizer->getData("POTCAR info").toList().isEmpty() || // No info at all!
          oldcomp != atomicNums // Composition has changed!
          ) {
        error("Using VASP and POTCAR is empty. Please select the pseudopotentials before continuing.");
        return;
      }

      // Build up the latest and greatest POTCAR compilation
      qobject_cast<VASPOptimizer*>(m_optimizer)->buildPOTCARs();
    }

    // Create the SSHManager
    if (m_optimizer->getIDString() != "GULP") { // GULP won't use ssh
      QString pw = "";
      for (;;) {
        try {
          m_ssh->makeConnections(host, username, pw, port);
        }
        catch (SSHConnection::SSHConnectionException e) {
          QString err;
          switch (e) {
          case SSHConnection::SSH_CONNECTION_ERROR:
          case SSHConnection::SSH_UNKNOWN_ERROR:
          default:
            err = "There was a problem connection to the ssh server at "
              + username + "@" + host + ":" + QString::number(port) + ". "
              + "Please check that all provided information is correct, "
              + "and attempt to log in outside of Avogadro before trying again.";
            error(err);
            return;
          case SSHConnection::SSH_UNKNOWN_HOST_ERROR: {
            // The host is not known, or has changed its key.
            // Ask user if this is ok.
            err = "The host "
              + host + ":" + QString::number(port)
              + " either has an unknown key, or has changed it's key:\n"
              + m_ssh->getServerKeyHash() + "\n"
              + "Would you like to trust the specified host?";
            bool ok;
            // This is a BlockingQueuedConnection, which blocks until
            // the slot returns.
            emit needBoolean(err, &ok);
            if (!ok) { // user cancels
              return;
            }
            m_ssh->validateServerKey();
            continue;
          } // end case
          case SSHConnection::SSH_BAD_PASSWORD_ERROR: {
            // Chances are that the pubkey auth was attempted but failed,
            // so just prompt user for password.
            err = "Please enter a password for "
              + username + "@" + host + ":" + QString::number(port)
              + ":";
            bool ok;
            QString newPassword;
            // This is a BlockingQueuedConnection, which blocks until
            // the slot returns.
            emit needPassword(err, &newPassword, &ok);
            if (!ok) { // user cancels
              return;
            }
            pw = newPassword;
            continue;
          } // end case
          } // end switch
        } // end catch
        break;
      } // end forever
    }

    // prepare pointers
    m_tracker->deleteAllStructures();

    ///////////////////////////////////////////////
    // Generate random structures and load seeds //
    ///////////////////////////////////////////////

    // Set up progress bar
    m_dialog->startProgressUpdate(tr("Generating structures..."), 0, 0);

    // Initalize loop variables
    int failed = 0;
    uint progCount = 0;
    QString filename;
    Xtal *xtal = 0;
    // Use new xtal count in case "addXtal" falls behind so that we
    // don't duplicate structures when switching from seeds -> random.
    uint newXtalCount=0;

    // Load seeds...
    for (int i = 0; i < seedList.size(); i++) {
      filename = seedList.at(i);
      Xtal *xtal = new Xtal;
      xtal->setFileName(filename);
      if ( !m_optimizer->read(xtal, filename) || (xtal == 0) ) {
        m_tracker->deleteAllStructures();
        error(tr("Error loading seed %1").arg(filename));
        return;
      }
      QString parents =tr("Seeded: %1", "1 is a filename").arg(filename);
      initializeAndAddXtal(xtal, 1, parents);
      debug(tr("XtalOpt::StartOptimization: Loaded seed: %1", "1 is a filename").arg(filename));
      m_dialog->updateProgressLabel(tr("%1 structures generated (%2 kept, %3 rejected)...").arg(i + failed).arg(i).arg(failed));
      newXtalCount++;
    }

    // Generation loop...
    for (uint i = newXtalCount; i < numInitial; i++) {
      // Update progress bar
      m_dialog->updateProgressMaximum( (i == 0)
                                        ? 0
                                        : int(progCount / static_cast<double>(i)) * numInitial );
      m_dialog->updateProgressValue(progCount);
      progCount++;
      m_dialog->updateProgressLabel(tr("%1 structures generated (%2 kept, %3 rejected)...").arg(i + failed).arg(i).arg(failed));

      // Generate/Check xtal
      xtal = generateRandomXtal(1, i+1);
      if (!checkXtal(xtal)) {
        delete xtal;
        i--;
        failed++;
      }
      else {
        xtal->findSpaceGroup(tol_spg);
        initializeAndAddXtal(xtal, 1, xtal->getParents());
        newXtalCount++;
      }
    }

    m_dialog->stopProgressUpdate();

    m_dialog->saveSession();
    emit sessionStarted();
  }

  Structure* XtalOpt::replaceWithRandom(Structure *s, const QString & reason) {
    Xtal *oldXtal = qobject_cast<Xtal*>(s);
    QWriteLocker locker1 (oldXtal->lock());

    // Generate/Check new xtal
    Xtal *xtal = 0;
    while (!checkXtal(xtal))
      xtal = generateRandomXtal(0, 0);

    // Copy info over
    QWriteLocker locker2 (xtal->lock());
    oldXtal->clear();
    oldXtal->setOBUnitCell(new OpenBabel::OBUnitCell);
    oldXtal->setCellInfo(xtal->OBUnitCell()->GetCellMatrix());
    oldXtal->resetEnergy();
    oldXtal->resetEnthalpy();
    oldXtal->setPV(0);
    oldXtal->setCurrentOptStep(1);
    QString parents = "Randomly generated";
    if (!reason.isEmpty())
      parents += " (" + reason + ")";
    oldXtal->setParents(parents);

    Atom *atom1, *atom2;
    for (uint i = 0; i < xtal->numAtoms(); i++) {
      atom1 = oldXtal->addAtom();
      atom2 = xtal->atom(i);
      atom1->setPos(atom2->pos());
      atom1->setAtomicNumber(atom2->atomicNumber());
    }
    oldXtal->findSpaceGroup(tol_spg);
    oldXtal->resetFailCount();

    // Delete random xtal
    xtal->deleteLater();
    return qobject_cast<Structure*>(oldXtal);
  }

  Xtal* XtalOpt::generateRandomXtal(uint generation, uint id) {
    INIT_RANDOM_GENERATOR();
    // Set cell parameters
    double a            = RANDDOUBLE() * (a_max-a_min) + a_min;
    double b            = RANDDOUBLE() * (b_max-b_min) + b_min;
    double c            = RANDDOUBLE() * (c_max-c_min) + c_min;
    double alpha        = RANDDOUBLE() * (alpha_max - alpha_min) + alpha_min;
    double beta         = RANDDOUBLE() * (beta_max  - beta_min ) + beta_min;
    double gamma        = RANDDOUBLE() * (gamma_max - gamma_min) + gamma_min;

    // Create crystal
    Xtal *xtal	= new Xtal(a, b, c, alpha, beta, gamma);
    QWriteLocker locker (xtal->lock());

    xtal->setStatus(Xtal::Empty);

    if (using_fixed_volume)
      xtal->setVolume(vol_fixed);

    // Populate crystal
    QList<uint> atomicNums = comp.keys();
    uint atomicNum;
    uint q;
    for (int num_idx = 0; num_idx < atomicNums.size(); num_idx++) {
      atomicNum = atomicNums.at(num_idx);
      q = comp.value(atomicNum);
      double IAD = (using_shortestInteratomicDistance)
                ? shortestInteratomicDistance
                : -1.0;
      for (uint i = 0; i < q; i++) {
        if (!xtal->addAtomRandomly(atomicNum, IAD)) {
          xtal->deleteLater();
          debug("XtalOpt::generateRandomXtal: Failed to add atoms with specified interatomic distance.");
          return 0;
        }
      }
    }

    // Set up geneology info
    xtal->setGeneration(generation);
    xtal->setIDNumber(id);
    xtal->setParents("Randomly generated");
    xtal->setStatus(Xtal::WaitingForOptimization);

    // Set up xtal data
    return xtal;
  }

  void XtalOpt::initializeAndAddXtal(Xtal *xtal, uint generation, const QString &parents) {
    xtalInitMutex->lock();
    QList<Structure*> allStructures = m_queue->lockForNaming();
    Structure *structure;
    uint id = 1;
    for (int j = 0; j < allStructures.size(); j++) {
      structure = allStructures.at(j);
      structure->lock()->lockForRead();
      if (structure->getGeneration() == generation &&
          structure->getIDNumber() >= id) {
        id = structure->getIDNumber() + 1;
      }
      structure->lock()->unlock();
    }

    QWriteLocker xtalLocker (xtal->lock());
    xtal->setIDNumber(id);
    xtal->setGeneration(generation);
    xtal->setParents(parents);
    QString id_s, gen_s, locpath_s, rempath_s;
    id_s.sprintf("%05d",xtal->getIDNumber());
    gen_s.sprintf("%05d",xtal->getGeneration());
    locpath_s = filePath + "/" + gen_s + "x" + id_s + "/";
    rempath_s = rempath + "/" + gen_s + "x" + id_s + "/";
    QDir dir (locpath_s);
    if (!dir.exists()) {
      if (!dir.mkpath(locpath_s)) {
        error(tr("XtalOpt::initializeAndAddXtal: Cannot write to path: %1 (path creation failure)",
                 "1 is a file path.")
              .arg(locpath_s));
      }
    }
    xtal->setFileName(locpath_s);
    xtal->setRempath(rempath_s);
    xtal->setCurrentOptStep(1);
    xtal->findSpaceGroup(tol_spg);
    m_queue->unlockForNaming(xtal);
    xtalInitMutex->unlock();
  }

  void XtalOpt::generateNewStructure()
  {
    QtConcurrent::run(this, &XtalOpt::generateNewStructure_);
  }

  void XtalOpt::generateNewStructure_()
  {
    INIT_RANDOM_GENERATOR();
    // Get all optimized structures
    QList<Structure*> structures = m_queue->getAllOptimizedStructures();

    // Check to see if there are enough optimized structure to perform
    // genetic operations
    if (structures.size() < 3) {
      Xtal *xtal = 0;
      while (!checkXtal(xtal)) {
        if (xtal) xtal->deleteLater();
        xtal = generateRandomXtal(1, 0);
      }
      initializeAndAddXtal(xtal, 1, xtal->getParents());
      return;
    }

    // Sort structure list
    Structure::sortByEnthalpy(&structures);

    // Trim list
    // Remove all but (n_consider + 1). The "+ 1" will be removed
    // during probability generation.
    while ( static_cast<uint>(structures.size()) > popSize + 1 )
      structures.removeLast();

    // Make list of weighted probabilities based on enthalpy values
    QList<double> probs = getProbabilityList(structures);

    QList<Xtal*> xtals;
    for (int i = 0; i < structures.size(); i++)
      xtals.append(qobject_cast<Xtal*>(structures.at(i)));

    // Initialize loop vars
    double r;
    unsigned int gen;
    QString parents;
    Xtal *xtal = 0;

    // Perform operation until xtal is valid:
    while (!checkXtal(xtal)) {
      // First delete any previous failed structure in xtal
      if (xtal) {
        delete xtal;
        xtal = 0;
      }

      // Decide operator:
      r = RANDDOUBLE();
      Operators op;
      if (r < p_cross/100.0)
        op = OP_Crossover;
      else if (r < (p_cross + p_strip)/100.0)
        op = OP_Stripple;
      else
        op = OP_Permustrain;

      // Try 1000 times to get a good structure from the selected
      // operation. If not possible, send a warning to the log and
      // start anew.
      int attemptCount = 0;
      while (attemptCount < 1000 && !checkXtal(xtal)) {
        attemptCount++;
        if (xtal) {
          delete xtal;
          xtal = 0;
        }

        // Operation specific set up:
        switch (op) {
        case OP_Crossover: {
          int ind1, ind2;
          Xtal *xtal1=0, *xtal2=0;
          // Select structures
          ind1 = ind2 = 0;
          double r1 = RANDDOUBLE();
          double r2 = RANDDOUBLE();
          for (ind1 = 0; ind1 < probs.size(); ind1++)
            if (r1 < probs.at(ind1)) break;
          for (ind2 = 0; ind2 < probs.size(); ind2++)
            if (r2 < probs.at(ind2)) break;

          xtal1 = xtals.at(ind1);
          xtal2 = xtals.at(ind2);

          // Perform operation
          double percent1;
          xtal = XtalOptGenetic::crossover(xtal1, xtal2, cross_minimumContribution, percent1);

          // Lock parents and get info from them
          xtal1->lock()->lockForRead();
          xtal2->lock()->lockForRead();
          uint gen1 = xtal1->getGeneration();
          uint gen2 = xtal2->getGeneration();
          uint id1 = xtal1->getIDNumber();
          uint id2 = xtal2->getIDNumber();
          xtal2->lock()->unlock();
          xtal1->lock()->unlock();

          // Determine generation number
          gen = ( gen1 >= gen2 ) ?
            gen1 + 1 :
            gen2 + 1 ;
          parents = tr("Crossover: %1x%2 (%3%) + %4x%5 (%6%)")
            .arg(gen1)
            .arg(id1)
            .arg(percent1, 0, 'f', 0)
            .arg(gen2)
            .arg(id2)
            .arg(100.0 - percent1, 0, 'f', 0);
          continue;
        }
        case OP_Stripple: {
          // Pick a parent
          int ind;
          double r = RANDDOUBLE();
          for (ind = 0; ind < probs.size(); ind++)
            if (r < probs.at(ind)) break;
          Xtal *xtal1 = xtals.at(ind);

          // Perform stripple
          double amplitude=0, stdev=0;
          xtal = XtalOptGenetic::stripple(xtal1,
                                          strip_strainStdev_min,
                                          strip_strainStdev_max,
                                          strip_amp_min,
                                          strip_amp_max,
                                          strip_per1,
                                          strip_per2,
                                          stdev,
                                          amplitude);

          // Lock parent and extract info
          xtal1->lock()->lockForRead();
          uint gen1 = xtal1->getGeneration();
          uint id1 = xtal1->getIDNumber();
          xtal1->lock()->unlock();

          // Determine generation number
          gen = gen1 + 1;
          parents = tr("Stripple: %1x%2 stdev=%3 amp=%4 waves=%5,%6")
            .arg(gen1)
            .arg(id1)
            .arg(stdev, 0, 'f', 5)
            .arg(amplitude, 0, 'f', 5)
            .arg(strip_per1)
            .arg(strip_per2);
          continue;
        }
        case OP_Permustrain: {
          int ind;
          double r = RANDDOUBLE();
          for (ind = 0; ind < probs.size(); ind++)
            if (r < probs.at(ind)) break;

          Xtal *xtal1 = xtals.at(ind);
          double stdev=0;
          xtal = XtalOptGenetic::permustrain(xtals.at(ind), perm_strainStdev_max, perm_ex, stdev);

          // Lock parent and extract info
          xtal1->lock()->lockForRead();
          uint gen1 = xtal1->getGeneration();
          uint id1 = xtal1->getIDNumber();
          xtal1->lock()->unlock();

          // Determine generation number
          gen = gen1 + 1;
          parents = tr("Permustrain: %1x%2 stdev=%3 exch=%4")
            .arg(gen1)
            .arg(id1)
            .arg(stdev, 0, 'f', 5)
            .arg(perm_ex);
          continue;
        }
        default:
          warning("XtalOpt::generateSingleOffspring: Attempt to use an invalid operator.");
        }
      }
      if (attemptCount >= 1000) {
        QString opStr;
        switch (op) {
        case OP_Crossover:   opStr = "crossover"; break;
        case OP_Stripple:    opStr = "stripple"; break;
        case OP_Permustrain: opStr = "permustrain"; break;
        default:             opStr = "(unknown)"; break;
        }
        warning(tr("Unable to perform operation %1 after 1000 tries. Reselecting operator...").arg(opStr));
      }
    }
    initializeAndAddXtal(xtal, gen, parents);
    return;
  }

  bool XtalOpt::checkLimits() {
    if (a_min > a_max) {
      warning("XtalOptRand::checkLimits error: Illogical A limits.");
      return false;
    }
    if (b_min > b_max) {
      warning("XtalOptRand::checkLimits error: Illogical B limits.");
      return false;
    }
    if (c_min > c_max) {
      warning("XtalOptRand::checkLimits error: Illogical C limits.");
      return false;
    }
    if (alpha_min > alpha_max) {
      warning("XtalOptRand::checkLimits error: Illogical Alpha limits.");
      return false;
    }
    if (beta_min > beta_max) {
      warning("XtalOptRand::checkLimits error: Illogical Beta limits.");
      return false;
    }
    if (gamma_min > gamma_max) {
      warning("XtalOptRand::checkLimits error: Illogical Gamma limits.");
      return false;
    }
    if (
        ( using_fixed_volume &&
          ( (a_min * b_min * c_min) > vol_fixed ||
            (a_max * b_max * c_max) < vol_fixed )
          ) ||
        ( !using_fixed_volume &&
          ( (a_min * b_min * c_min) > vol_max ||
            (a_max * b_max * c_max) < vol_min ||
            vol_min > vol_max)
          )) {
      warning("XtalOptRand::checkLimits error: Illogical Volume limits. (Also check min/max volumes based on cell lengths)");
      return false;
    }
    return true;
  }

  bool XtalOpt::checkXtal(Xtal *xtal) {
    if (!xtal) {
      return false;
    }

    // Lock xtal
    QWriteLocker locker (xtal->lock());

    if (xtal->getStatus() == Xtal::Empty) return false;

    // Check volume
    if (using_fixed_volume) {
      locker.unlock();
      xtal->setVolume(vol_fixed);
      locker.relock();
    }
    else if ( xtal->getVolume() < vol_min ||
              xtal->getVolume() > vol_max ) {
      // I don't want to initialize a random number generator here, so
      // just use the modulus of the current volume as a random float.
      double newvol = fabs(fmod(xtal->getVolume(), 1)) * (vol_max - vol_min) + vol_min;
      if (fabs(newvol) < 1e-8) newvol = (vol_max - vol_min)*0.5 + vol_min;
      qDebug() << "XtalOpt::checkXtal: Rescaling volume from "
               << xtal->getVolume() << " to " << newvol;
      xtal->setVolume(newvol);
    }

    // Scale to any fixed parameters
    double a, b, c, alpha, beta, gamma;
    a = b = c = alpha = beta = gamma = 0;
    if (a_min == a_max) a = a_min;
    if (b_min == b_max) b = b_min;
    if (c_min == c_max) c = c_min;
    if (alpha_min ==    alpha_max)      alpha = alpha_min;
    if (beta_min ==     beta_max)       beta = beta_min;
    if (gamma_min ==    gamma_max)      gamma = gamma_min;
    xtal->rescaleCell(a, b, c, alpha, beta, gamma);

    // Before fixing angles, make sure that the current cell
    // parameters are realistic
    if (GS_IS_NAN_OR_INF(xtal->getA()) || fabs(xtal->getA()) < 1e-8 ||
        GS_IS_NAN_OR_INF(xtal->getB()) || fabs(xtal->getB()) < 1e-8 ||
        GS_IS_NAN_OR_INF(xtal->getC()) || fabs(xtal->getC()) < 1e-8 ||
        GS_IS_NAN_OR_INF(xtal->getAlpha()) || fabs(xtal->getAlpha()) < 1e-8 ||
        GS_IS_NAN_OR_INF(xtal->getBeta())  || fabs(xtal->getBeta())  < 1e-8 ||
        GS_IS_NAN_OR_INF(xtal->getGamma()) || fabs(xtal->getGamma()) < 1e-8 ) {
      qDebug() << "XtalOpt::checkXtal: A cell parameter is either 0, nan, or inf. Discarding.";
      return false;
    }

    // Ensure that all angles are between 60 and 120:
    xtal->fixAngles();

    // Check lattice
    if ( ( !a     && ( xtal->getA() < a_min         || xtal->getA() > a_max         ) ) ||
         ( !b     && ( xtal->getB() < b_min         || xtal->getB() > b_max         ) ) ||
         ( !c     && ( xtal->getC() < c_min         || xtal->getC() > c_max         ) ) ||
         ( !alpha && ( xtal->getAlpha() < alpha_min || xtal->getAlpha() > alpha_max ) ) ||
         ( !beta  && ( xtal->getBeta()  < beta_min  || xtal->getBeta()  > beta_max  ) ) ||
         ( !gamma && ( xtal->getGamma() < gamma_min || xtal->getGamma() > gamma_max ) ) )  {
      qDebug() << "Discarding structure -- Bad lattice:" <<endl
               << "A:     " << a_min << " " << xtal->getA() << " " << a_max << endl
               << "B:     " << b_min << " " << xtal->getB() << " " << b_max << endl
               << "C:     " << c_min << " " << xtal->getC() << " " << c_max << endl
               << "Alpha: " << alpha_min << " " << xtal->getAlpha() << " " << alpha_max << endl
               << "Beta:  " << beta_min  << " " << xtal->getBeta()  << " " << beta_max << endl
               << "Gamma: " << gamma_min << " " << xtal->getGamma() << " " << gamma_max;
      return false;
    }

    // Check interatomic distances
    if (using_shortestInteratomicDistance) {
      double distance = 0;
      if (xtal->getShortestInteratomicDistance(distance)) {
        if (distance < shortestInteratomicDistance) {
          qDebug() << "Discarding structure -- Bad IAD ("
                   << distance << " < "
                   << shortestInteratomicDistance << ")";
          return false;
        }
      }
    }

    // Xtal is OK!
    return true;
  }

  QString XtalOpt::interpretTemplate(const QString & templateString, Structure* structure)
  {
    QStringList list = templateString.split("%");
    QString line;
    QString origLine;
    Xtal *xtal = qobject_cast<Xtal*>(structure);
    for (int line_ind = 0; line_ind < list.size(); line_ind++) {
      origLine = line = list.at(line_ind);
      interpretKeyword_base(line, structure);
      interpretKeyword(line, structure);
      if (line != origLine) { // Line was a keyword
        list.replace(line_ind, line);
      }
    }
    // Rejoin string
    QString ret = list.join("");
    ret += "\n";
    return ret;
  }

  void XtalOpt::interpretKeyword(QString &line, Structure* structure)
  {
    QString rep = "";
    Xtal *xtal = qobject_cast<Xtal*>(structure);

    // Xtal specific keywords
    if (line == "a")                    rep += QString::number(xtal->getA());
    else if (line == "b")               rep += QString::number(xtal->getB());
    else if (line == "c")               rep += QString::number(xtal->getC());
    else if (line == "alphaRad")        rep += QString::number(xtal->getAlpha() * DEG_TO_RAD);
    else if (line == "betaRad")         rep += QString::number(xtal->getBeta() * DEG_TO_RAD);
    else if (line == "gammaRad")        rep += QString::number(xtal->getGamma() * DEG_TO_RAD);
    else if (line == "alphaDeg")        rep += QString::number(xtal->getAlpha());
    else if (line == "betaDeg")         rep += QString::number(xtal->getBeta());
    else if (line == "gammaDeg")        rep += QString::number(xtal->getGamma());
    else if (line == "volume")          rep += QString::number(xtal->getVolume());
    else if (line == "coordsFrac") {
      OpenBabel::OBMol obmol = xtal->OBMol();
      FOR_ATOMS_OF_MOL(atom, obmol) {
        vector3 coords = xtal->cartToFrac(atom->GetVector());
        rep += static_cast<QString>(OpenBabel::etab.GetSymbol(atom->GetAtomicNum())) + " ";
        rep += QString::number(coords.x()) + " ";
        rep += QString::number(coords.y()) + " ";
        rep += QString::number(coords.z()) + "\n";
      }
    }
    else if (line == "coordsFracId") {
      OpenBabel::OBMol obmol = xtal->OBMol();
      FOR_ATOMS_OF_MOL(atom, obmol) {
        vector3 coords = xtal->cartToFrac(atom->GetVector());
        rep += static_cast<QString>(OpenBabel::etab.GetSymbol(atom->GetAtomicNum())) + " ";
        rep += QString::number(atom->GetAtomicNum()) + " ";
        rep += QString::number(coords.x()) + " ";
        rep += QString::number(coords.y()) + " ";
        rep += QString::number(coords.z()) + "\n";
      }
    }
    else if (line == "cellMatrixAngstrom") {
      matrix3x3 m = xtal->OBUnitCell()->GetCellMatrix();
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          rep += QString::number(m.Get(i,j)) + "\t";
        }
        rep += "\n";
      }
    }
    else if (line == "cellVector1Angstrom") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[0];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i]) + "\t";
      }
    }
    else if (line == "cellVector2Angstrom") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[1];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i]) + "\t";
      }
    }
    else if (line == "cellVector3Angstrom") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[2];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i]) + "\t";
      }
    }
    else if (line == "cellMatrixBohr") {
      matrix3x3 m = xtal->OBUnitCell()->GetCellMatrix();
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          rep += QString::number(m.Get(i,j) * ANGSTROM_TO_BOHR) + "\t";
        }
        rep += "\n";
      }
    }
    else if (line == "cellVector1Bohr") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[0];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i] * ANGSTROM_TO_BOHR) + "\t";
      }
    }
    else if (line == "cellVector2Bohr") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[1];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i] * ANGSTROM_TO_BOHR) + "\t";
      }
    }
    else if (line == "cellVector3Bohr") {
      vector3 v = xtal->OBUnitCell()->GetCellVectors()[2];
      for (int i = 0; i < 3; i++) {
        rep += QString::number(v[i] * ANGSTROM_TO_BOHR) + "\t";
      }
    }
    else if (line == "POSCAR") {
      // Comment line -- set to filename
      rep += xtal->fileName();
      rep += "\n";
      // Scaling factor. Just 1.0
      rep += QString::number(1.0);
      rep += "\n";
      // Unit Cell Vectors
      std::vector< vector3 > vecs = xtal->OBUnitCell()->GetCellVectors();
      for (uint i = 0; i < vecs.size(); i++) {
        rep += QString::number(vecs.at(i).x()) + " ";
        rep += QString::number(vecs.at(i).y()) + " ";
        rep += QString::number(vecs.at(i).z()) + " ";
        rep += "\n";
      }
      // Number of each type of atom (sorted alphabetically by symbol)
      QList<uint> list = xtal->getNumberOfAtomsAlpha();
      for (int i = 0; i < list.size(); i++) {
        rep += QString::number(list.at(i)) + " ";
      }
      rep += "\n";
      // Use fractional coordinates:
      rep += "Direct\n";
      // Coordinates of each atom (sorted alphabetically by symbol)
      QList<Eigen::Vector3d> coords = xtal->getAtomCoordsFrac();
      for (int i = 0; i < coords.size(); i++) {
        rep += QString::number(coords.at(i).x()) + " ";
        rep += QString::number(coords.at(i).y()) + " ";
        rep += QString::number(coords.at(i).z()) + " ";
        rep += "\n";
      }
    } // End %POSCAR%

    if (!rep.isEmpty()) {
      // Remove any trailing newlines
      rep = rep.replace(QRegExp("\n$"), "");
      line = rep;
    }
  }

  QString XtalOpt::getTemplateKeywordHelp()
  {
    QString help = "";
    help.append(getTemplateKeywordHelp_base());
    help.append("\n");
    help.append(getTemplateKeywordHelp_xtalopt());
    return help;
  }

  QString XtalOpt::getTemplateKeywordHelp_xtalopt()
  {
    QString str;
    QTextStream out (&str);
    out
      << "Crystal specific information:\n"
      << "%POSCAR% -- VASP poscar generator\n"
      << "%coordsFrac% -- fractional coordinate data\n\t[symbol] [x] [y] [z]\n"
      << "%coordsFracId% -- fractional coordinate data with atomic number\n\t[symbol] [atomic number] [x] [y] [z]\n"
      << "%cellMatrixAngstrom% -- Cell matrix in Angstrom\n"
      << "%cellVector1Angstrom% -- First cell vector in Angstrom\n"
      << "%cellVector2Angstrom% -- Second cell vector in Angstrom\n"
      << "%cellVector3Angstrom% -- Third cell vector in Angstrom\n"
      << "%cellMatrixBohr% -- Cell matrix in Bohr\n"
      << "%cellVector1Bohr% -- First cell vector in Bohr\n"
      << "%cellVector2Bohr% -- Second cell vector in Bohr\n"
      << "%cellVector3Bohr% -- Third cell vector in Bohr\n"
      << "%a% -- Lattice parameter A\n"
      << "%b% -- Lattice parameter B\n"
      << "%c% -- Lattice parameter C\n"
      << "%alphaRad% -- Lattice parameter Alpha in rad\n"
      << "%betaRad% -- Lattice parameter Beta in rad\n"
      << "%gammaRad% -- Lattice parameter Gamma in rad\n"
      << "%alphaDeg% -- Lattice parameter Alpha in degrees\n"
      << "%betaDeg% -- Lattice parameter Beta in degrees\n"
      << "%gammaDeg% -- Lattice parameter Gamma in degrees\n"
      << "%volume% -- Unit cell volume\n"
      << "%gen% -- xtal generation number\n"
      << "%id% -- xtal id number\n";

    return str;
  }

  bool XtalOpt::load(const QString &filename, const bool forceReadOnly) {
    if (forceReadOnly) {
      readOnly = true;
    }

    // Attempt to open state file
    QFile file (filename);
    if (!file.open(QIODevice::ReadOnly)) {
      error("XtalOpt::load(): Error opening file "+file.fileName()+" for reading...");
      return false;
    }

    SETTINGS(filename);
    int loadedVersion = settings->value("xtalopt/version", 0).toInt();

    // Update config data
    switch (loadedVersion) {
    case 0:
    case 1:
    default:
      break;
    }

    bool stateFileIsValid = settings->value("xtalopt/saveSuccessful",
                                            false).toBool();
    if (!stateFileIsValid) {
      error("XtalOpt::load(): File "+file.fileName()+
            " is incomplete, corrupt, or invalid. (Try "
            + file.fileName() + ".old if it exists)");
      return false;
    }

    // Get path and other info for later:
    QFileInfo stateInfo (file);
    // path to resume file
    QDir dataDir  = stateInfo.absoluteDir();
    QString dataPath = dataDir.absolutePath() + "/";
    // list of xtal dirs
    QStringList xtalDirs = dataDir.entryList(QStringList(),
                                             QDir::AllDirs,
                                             QDir::Size);
    xtalDirs.removeAll(".");
    xtalDirs.removeAll("..");
    for (int i = 0; i < xtalDirs.size(); i++) {
      // old versions of xtalopt used xtal.state, so still check for it.
      if (!QFile::exists(dataPath + "/" + xtalDirs.at(i)
                         + "/structure.state") &&
          !QFile::exists(dataPath + "/" + xtalDirs.at(i)
                         + "/xtal.state") ) {
          xtalDirs.removeAt(i);
          i--;
      }
    }

    // Set filePath:
    QString newFilePath = dataPath;
    QString newFileBase = filename;
    newFileBase.remove(newFilePath);
    newFileBase.remove("xtalopt.state.old");
    newFileBase.remove("xtalopt.state.tmp");
    newFileBase.remove("xtalopt.state");

    // TODO For some reason, the local view of "this" is not changed
    // when the settings are loaded in the following line. The tabs
    // are loading the settings and setting the variables in their
    // scope, but it isn't changing it here. Caching issue maybe?
    m_dialog->readSettings(filename);

    // Set optimizer
    setOptimizer(OptTypes(settings->value("xtalopt/edit/optType").toInt()),
                 filename);

    // Create SSHConnection
    if (!forceReadOnly && m_optimizer->getIDString() != "GULP") { // GULP won't use ssh
      QString pw = "";
      for (;;) {
        try {
          m_ssh->makeConnections(host, username, pw, port);
        }
        catch (SSHConnection::SSHConnectionException e) {
          QString err;
          switch (e) {
          case SSHConnection::SSH_CONNECTION_ERROR:
          case SSHConnection::SSH_UNKNOWN_ERROR:
          default:
            err = "There was a problem connection to the ssh server at "
              + username + "@" + host + ":" + QString::number(port) + ". "
              + "Please check that all provided information is correct, "
              + "and attempt to log in outside of Avogadro before trying again."
              + "XtalOpt will continue to load in read-only mode.";
            error(err);
            readOnly = true;
            break;
          case SSHConnection::SSH_UNKNOWN_HOST_ERROR: {
            // The host is not known, or has changed its key.
            // Ask user if this is ok.
            err = "The host "
              + host + ":" + QString::number(port)
              + " either has an unknown key, or has changed it's key:\n"
              + m_ssh->getServerKeyHash() + "\n"
              + "Would you like to trust the specified host? (Clicking 'No' will"
              + "resume the session in read only mode.)";
            bool ok;
            // Commenting this until ticket:53 (load in bg thread) is fixed
            // // This is a BlockingQueuedConnection, which blocks until
            // // the slot returns.
            // emit needPassword(err, &newPassword, &ok);
            promptForBoolean(err, &ok);
            if (!ok) { // user cancels
              readOnly = true;
              break;
            }
            m_ssh->validateServerKey();
            continue;
          } // end case
          case SSHConnection::SSH_BAD_PASSWORD_ERROR: {
            // Chances are that the pubkey auth was attempted but failed,
            // so just prompt user for password.
            err = "Please enter a password for "
              + username + "@" + host + ":" + QString::number(port)
              + " or cancel to load the session in read-only mode.";
            bool ok;
            QString newPassword;
            // Commenting this until ticket:53 (load in bg thread) is fixed
            // // This is a BlockingQueuedConnection, which blocks until
            // // the slot returns.
            // emit needPassword(err, &newPassword, &ok);
            promptForPassword(err, &newPassword, &ok);
            if (!ok) { // user cancels
              readOnly = true;
              break;
            }
            pw = newPassword;
            continue;
          } // end case
          } // end switch
        } // end catch
        break;
      } // end forever
    }

    debug(tr("Resuming XtalOpt session in '%1' (%2) readOnly = %3")
          .arg(filename)
          .arg(m_optimizer->getIDString())
          .arg( (readOnly) ? "true" : "false"));

    // Xtals
    // Initialize progress bar:
    m_dialog->updateProgressMaximum(xtalDirs.size());
    Xtal* xtal;
    QList<uint> keys = comp.keys();
    QList<Structure*> loadedStructures;
    QString xtalStateFileName;
    uint count = 0;
    int numDirs = xtalDirs.size();
    for (int i = 0; i < numDirs; i++) {
      count++;
      m_dialog->updateProgressLabel(tr("Loading structures(%1 of %2)...").arg(count).arg(numDirs));
      m_dialog->updateProgressValue(count-1);

      xtalStateFileName = dataPath + "/" + xtalDirs.at(i) + "/structure.state";
      // Check if this is an older session that used xtal.state instead.
      if ( !QFile::exists(xtalStateFileName) &&
           QFile::exists(dataPath + "/" + xtalDirs.at(i) + "/xtal.state") ) {
        xtalStateFileName = dataPath + "/" + xtalDirs.at(i) + "/xtal.state";
      }

      xtal = new Xtal();
      QWriteLocker locker (xtal->lock());
      // Add empty atoms to xtal, updateXtal will populate it
      for (int j = 0; j < keys.size(); j++) {
        for (uint k = 0; k < comp.value(keys.at(j)); k++)
          xtal->addAtom();
      }
      xtal->setFileName(dataPath + "/" + xtalDirs.at(i) + "/");
      xtal->readSettings(xtalStateFileName);

      // Store current state -- updateXtal will overwrite it.
      Xtal::State state = xtal->getStatus();
      QDateTime endtime = xtal->getOptTimerEnd();

      locker.unlock();

      if (!m_optimizer->load(xtal)) {
        error(tr("Error, no (or not appropriate for %1) xtal data in %2.\n\nThis could be a result of resuming a structure that has not yet done any local optimizations. If so, safely ignore this message.")
              .arg(m_optimizer->getIDString())
              .arg(xtal->fileName()));
        continue;
      }

      // Reset state
      locker.relock();
      xtal->setStatus(state);
      xtal->setOptTimerEnd(endtime);
      locker.unlock();
      loadedStructures.append(qobject_cast<Structure*>(xtal));
    }

    m_dialog->updateProgressMinimum(0);
    m_dialog->updateProgressValue(0);
    m_dialog->updateProgressMaximum(loadedStructures.size());
    m_dialog->updateProgressLabel("Sorting and checking structures...");

    // Sort Xtals by index values
    int curpos = 0;
    //dialog->stopProgressUpdate();
    //dialog->startProgressUpdate("Sorting xtals...", 0, loadedStructures.size()-1);
    for (int i = 0; i < loadedStructures.size(); i++) {
      m_dialog->updateProgressValue(i);
      for (int j = 0; j < loadedStructures.size(); j++) {
        //dialog->updateProgressValue(curpos);
        if (loadedStructures.at(j)->getIndex() == i) {
          loadedStructures.swap(j, curpos);
          curpos++;
        }
      }
    }

    m_dialog->updateProgressMinimum(0);
    m_dialog->updateProgressValue(0);
    m_dialog->updateProgressMaximum(loadedStructures.size());
    m_dialog->updateProgressLabel("Updating structure indices...");

    // Reassign indices (shouldn't always be necessary, but just in case...)
    for (int i = 0; i < loadedStructures.size(); i++) {
      m_dialog->updateProgressValue(i);
      loadedStructures.at(i)->setIndex(i);
    }

    m_dialog->updateProgressMinimum(0);
    m_dialog->updateProgressValue(0);
    m_dialog->updateProgressMaximum(loadedStructures.size());
    m_dialog->updateProgressLabel("Preparing GUI and tracker...");

    // Reset the local file path information in case the files have moved
    filePath = newFilePath;

    Structure *s= 0;
    for (int i = 0; i < loadedStructures.size(); i++) {
      s = loadedStructures.at(i);
      m_dialog->updateProgressValue(i);
      m_tracker->append(s);
      if (s->getStatus() == Structure::WaitingForOptimization)
        m_queue->appendToJobStartTracker(s);
    }

    m_dialog->updateProgressLabel("Done!");

    // Check if user wants to resume the search
    if (!readOnly) {
      bool resume;
      // TODO Change this to needBoolean once reload in move to bg thread
      promptForBoolean(tr("Session '%1' (%2) loaded. Would you like to start submitting jobs and resume the search? (Answering \"No\" will enter read-only mode.)")
                       .arg(description).arg(filePath),
                       &resume);

      readOnly = !resume;
      qDebug() << "Read only? " << readOnly;

      // Start search if needed
      if (!readOnly) {
	qobject_cast<XtalOptDialog*>(m_dialog)->startProgressTimer();
      }
    }

    return true;
  }

  void XtalOpt::resetDuplicates() {
    if (isStarting) {
      return;
    }
    QtConcurrent::run(this, &XtalOpt::resetDuplicates_);
  }

  void XtalOpt::resetDuplicates_() {
    QList<Structure*> *structures = m_tracker->list();
    Xtal *xtal = 0;
    for (int i = 0; i < structures->size(); i++) {
      xtal = qobject_cast<Xtal*>(structures->at(i));
      xtal->lock()->lockForWrite();
      xtal->findSpaceGroup(tol_spg);
      if (xtal->getStatus() == Xtal::Duplicate)
        xtal->setStatus(Xtal::Optimized);
      xtal->lock()->unlock();
    }
    checkForDuplicates();
    emit updateAllInfo();
  }

  void XtalOpt::checkForDuplicates() {
    if (isStarting) {
      return;
    }
    QtConcurrent::run(this, &XtalOpt::checkForDuplicates_);
  }

  void XtalOpt::checkForDuplicates_() {
    QHash<QString, double> limits;
    limits.insert("enthalpy", tol_enthalpy);
    limits.insert("volume", tol_volume);

    QList<QString> keys = limits.keys();
    QList<QHash<QString, QVariant> > fps;
    QList<Xtal::State> states;

    m_tracker->lockForRead();
    QList<Structure*> *structures = m_tracker->list();

    Xtal *xtal=0, *xtal_i=0, *xtal_j=0;
    for (int i = 0; i < structures->size(); i++) {
      xtal = qobject_cast<Xtal*>(structures->at(i));
      xtal->lock()->lockForRead();
      fps.append(xtal->getFingerprint());
      states.append(xtal->getStatus());
      xtal->lock()->unlock();
    }

    // Iterate over all xtals
    const QHash<QString, QVariant> *fp_i, *fp_j;
    QString key;
    for (int i = 0; i < fps.size(); i++) {
      if ( states.at(i) != Xtal::Optimized ) continue;
      fp_i = &fps.at(i);
      // skip unknown spacegroups
      if (fp_i->value("spacegroup").toUInt() == 0) continue;
      for (int j = i+1; j < fps.size(); j++) {
        if (states.at(j) != Xtal::Optimized ) continue;
        fp_j = &fps.at(j);
        // skip unknown spacegroups
        if (fp_j->value("spacegroup").toUInt() == 0) continue;
        // If xtals do not have the same spacegroup number, break
        if (fp_i->value("spacegroup").toUInt() != fp_j->value("spacegroup").toUInt()) {
          continue;
        }
        // Check limits
        bool match = true;
        for (int k = 0; k < keys.size(); k++) {
          key = keys.at(k);
          // If values do not match, skip to next pair of xtals.
          if (fabs(fp_i->value(key).toDouble() - fp_j->value(key).toDouble() )
              > limits.value(key)) {
            match = false;
            break;
          }
        }
        if (!match) continue;
        // If we get here, all the fingerprint values match,
        // and we have a duplicate. Mark the xtal with the
        // highest enthalpy as a duplicate of the other.
        xtal_i = qobject_cast<Xtal*>(structures->at(i));
        xtal_j = qobject_cast<Xtal*>(structures->at(j));
        if (fp_i->value("enthalpy").toDouble() > fp_j->value("enthalpy").toDouble()) {
          xtal_i->lock()->lockForWrite();
          xtal_j->lock()->lockForRead();
          xtal_i->setStatus(Xtal::Duplicate);
          xtal_i->setDuplicateString(QString("%1x%2")
                                     .arg(xtal_j->getGeneration())
                                     .arg(xtal_j->getIDNumber()));
          xtal_i->lock()->unlock();
          xtal_j->lock()->unlock();
          break; // If xtals->at(i) is now a duplicate, don't bother comparing it anymore
        }
        else {
          xtal_j->lock()->lockForWrite();
          xtal_i->lock()->lockForRead();
          xtal_j->setStatus(Xtal::Duplicate);
          xtal_j->setDuplicateString(QString("%1x%2")
                                     .arg(xtal_i->getGeneration())
                                     .arg(xtal_i->getIDNumber()));
          xtal_j->lock()->unlock();
          xtal_i->lock()->unlock();
        }
      }
    }
    m_tracker->unlock();
    emit updateAllInfo();
  }

  void XtalOpt::setOptimizer_string(const QString &IDString, const QString &filename)
  {
    if (IDString.toLower() == "vasp")
      setOptimizer(new VASPOptimizer (this, filename));
    else if (IDString.toLower() == "gulp")
      setOptimizer(new GULPOptimizer (this, filename));
    else if (IDString.toLower() == "pwscf")
      setOptimizer(new PWscfOptimizer (this, filename));
    else if (IDString.toLower() == "castep")
      setOptimizer(new CASTEPOptimizer (this, filename));
    else
      error(tr("XtalOpt::setOptimizer: unable to determine optimizer from '%1'")
            .arg(IDString));
  }

  void XtalOpt::setOptimizer_enum(OptTypes opttype, const QString &filename)
  {
    switch (opttype) {
    case OT_VASP:
      setOptimizer(new VASPOptimizer (this, filename));
      break;
    case OT_GULP:
      setOptimizer(new GULPOptimizer (this, filename));
      break;
    case OT_PWscf:
      setOptimizer(new PWscfOptimizer (this, filename));
      break;
    case OT_CASTEP:
      setOptimizer(new CASTEPOptimizer (this, filename));
      break;
    default:
      error(tr("XtalOpt::setOptimizer: unable to determine optimizer from '%1'")
            .arg(QString::number((int)opttype)));
      break;
    }
  }

} // end namespace Avogadro

//#include "xtalopt.moc"
