#include "ci_kilobot_controller.h"
#include <argos3/core/utility/logging/argos_log.h>
#include <argos3/core/simulator/physics_engine/physics_engine.h>

/****************************************/
/****************************************/

CCI_KilobotController::CCI_KilobotController() :
   m_ptRobotState(NULL),
   m_pcMotors(NULL),
   m_pcLED(NULL),
   m_pcLight(NULL),
   m_pcCommA(NULL),
   m_pcCommS(NULL),
   m_pcRNG(NULL),
   m_nSharedMemFD(-1),
   m_nDebugInfoFD(-1),
   m_tBehaviorPID(-1) {}

/****************************************/
/****************************************/

void CCI_KilobotController::Init(TConfigurationNode& t_tree) {
   try {
      /* Initialize devices */
      try {
         m_pcMotors = GetActuator<CCI_DifferentialSteeringActuator>("differential_steering");
      } catch(CARGoSException&) {}
      try {
         m_pcLED    = GetActuator<CCI_LEDsActuator                >("leds"                 );
      } catch(CARGoSException&) {}
      try {
         m_pcCommA  = GetActuator<CCI_KilobotCommunicationActuator>("kilobot_communication");
      } catch(CARGoSException&) {}
      try {
         m_pcCommS  = GetSensor  <CCI_KilobotCommunicationSensor  >("kilobot_communication");
      } catch(CARGoSException&) {}
      try {
         m_pcLight  = GetSensor  <CCI_KilobotLightSensor          >("kilobot_light"        );
      } catch(CARGoSException&) {}
      /* Parse XML parameters */
      std::string strBehavior;
      GetNodeAttribute(t_tree, "behavior", strBehavior);
      /* Make sure script file exists */
      int nBehaviorFD = open(strBehavior.c_str(), O_RDONLY);
      if(nBehaviorFD < 0) {
         THROW_ARGOSEXCEPTION("Opening behavior file \"" << strBehavior << "\": " << strerror(errno));
      }
      close(nBehaviorFD);
      /* Create a random number generator */
      m_pcRNG = CRandom::CreateRNG("argos");
      /* Create shared memory area for master-slave communication */
      pid_t tParentPID = getpid();
      m_nSharedMemFD = ::shm_open(("/" + ToString<pid_t>(tParentPID) + "_" + GetId()).c_str(),
                                  O_RDWR | O_CREAT,
                                  S_IRUSR | S_IWUSR);
      if(m_nSharedMemFD < 0) {
         THROW_ARGOSEXCEPTION("Creating a shared memory area for " << GetId() << ": " << ::strerror(errno));
      }
      /* Resize shared memory area to contain the robot state */
      ::ftruncate(m_nSharedMemFD, sizeof(kilobot_state_t));
      /* Get pointer to shared memory area */
      m_ptRobotState = reinterpret_cast<kilobot_state_t*>(
         ::mmap(NULL,
                sizeof(kilobot_state_t),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                m_nSharedMemFD,
                0));
      if(m_ptRobotState == MAP_FAILED) {
         THROW_ARGOSEXCEPTION("Mmapping the shared memory area of " << GetId() << ": " << ::strerror(errno));
      }
      ::memset(m_ptRobotState, 0, sizeof(kilobot_state_t));
      /* Fork this process */
      m_tBehaviorPID = ::fork();
      if(m_tBehaviorPID < 0) {
         THROW_ARGOSEXCEPTION("Forking the behavior process of " << GetId() << ": " << ::strerror(errno));
      }
      /* Execute the behavior */
      if(m_tBehaviorPID == 0) {
         ::execl(strBehavior.c_str(),
                 strBehavior.c_str(),                                                 // Script name
                 ToString(tParentPID).c_str(),                                        // The parent process' PID
                 GetId().c_str(),                                                     // Robot id
                 ToString(static_cast<UInt32>(CPhysicsEngine::GetSimulationClockTick() * 1000)).c_str(), // Control step duration in ms
                 ToString(m_pcRNG->Uniform(CRange<UInt32>(0, 0xFFFFFFFFUL))).c_str(), // Random seed for rand_hard()
                 NULL
            );
         /* If the next line is executed, it's because execl did not succeed */
         THROW_ARGOSEXCEPTION("Executing the behavior process of " << GetId() << ": " << strBehavior << ": " << ::strerror(errno));
         ::exit(1);
      }
   }
   catch(CARGoSException& ex) {
      THROW_ARGOSEXCEPTION_NESTED("Error initializing the Kilobot controller for robot " << GetId(), ex);
   }
}

/****************************************/
/****************************************/

void CCI_KilobotController::ControlStep() {
   /* Set light reading */
   if(m_pcLight)
      m_ptRobotState->ambientlight = m_pcLight->GetReading();
   /* Set received message */
   if(m_pcCommS) {
      if(!m_pcCommS->GetPackets().empty()) {
         m_ptRobotState->rx_state = Min<UInt8>(m_pcCommS->GetPackets().size(), KILOBOT_MAX_RX);
         for(size_t i = 0; i < m_ptRobotState->rx_state; ++i) {
            ::memcpy(&m_ptRobotState->rx_message[i],
                     m_pcCommS->GetPackets()[i].Message,
                     sizeof(message_t));
            ::memcpy(&m_ptRobotState->rx_distance[i],
                     &m_pcCommS->GetPackets()[i].Distance,
                     sizeof(distance_measurement_t));
         }
      }
      /* Was last message sent? */
      if(m_pcCommS->MessageSent()) {
         m_ptRobotState->tx_state = 2;
      }
   }
   // TODO m_ptRobotState->voltage
   // TODO m_ptRobotState->temperature
   /* Resume process */
   ::kill(m_tBehaviorPID, SIGCONT);
   /* Wait for behavior to be done */
   ::waitpid(m_tBehaviorPID, NULL, WUNTRACED);
   /* Set actuator values */
   // TODO set proper conversion factors
   if(m_pcMotors) {
      m_pcMotors->SetLinearVelocity(3.0 * m_ptRobotState->right_motor / 255.0,
                                    3.0 * m_ptRobotState->left_motor / 255.0);
   }
   if(m_pcLED) {
      m_pcLED->SetSingleColor(0, CColor(255 * RED(m_ptRobotState->color)   / 3,
                                        255 * GREEN(m_ptRobotState->color) / 3,
                                        255 * BLUE(m_ptRobotState->color)  / 3));
   }
   /* Set message to send */
   if(m_pcCommA && m_ptRobotState->tx_state == 1) {
      m_pcCommA->SetMessage(&m_ptRobotState->tx_message);
   }
}

/****************************************/
/****************************************/

void CCI_KilobotController::Reset() {
   ::memset(m_ptRobotState, 0, sizeof(kilobot_state_t));
}

/****************************************/
/****************************************/

void CCI_KilobotController::Destroy() {
   ::kill(m_tBehaviorPID, SIGTERM);
   ::kill(m_tBehaviorPID, SIGCONT);
   int nStatus;
   ::waitpid(m_tBehaviorPID, &nStatus, WIFEXITED(nStatus));
   munmap(m_ptRobotState, sizeof(kilobot_state_t));
   close(m_nSharedMemFD);
   pid_t tParentPID = getpid();
   ::shm_unlink(("/" + ToString<pid_t>(tParentPID) + "_" + GetId()).c_str());
}

/****************************************/
/****************************************/

REGISTER_CONTROLLER(CCI_KilobotController, "kilobot_controller");
