/*
 * Copyright (C) 2018 Fondazione Istituto Italiano di Tecnologia iCub Facility
 * Authors: see AUTHORS file.
 * CopyPolicy: Released under the terms of the LGPLv2.1 or any later version, see LGPL.TXT or LGPL3.TXT
 */

// gazebo
#include <gazebo/physics/Model.hh>
#include <gazebo/physics/Link.hh>
#include <gazebo/physics/World.hh>
#include <gazebo/sensors/sensors.hh>

#include <gazebo/physics/physics.hh>

// ignition
#include <ignition/math/Pose3.hh>
#include <ignition/math/Vector3.hh>

// GazeboYarpPlugins
#include <GazeboYarpPlugins/common.h>
#include <GazeboYarpPlugins/ConfHelpers.hh>

// yarp
#include <yarp/os/Log.h>
#include <yarp/os/LogStream.h>
#include <yarp/os/Value.h>
#include <yarp/os/Searchable.h>
#include <yarp/math/FrameTransform.h>

// icub-main
#include <iCub/skinDynLib/skinContact.h>
#include <iCub/skinDynLib/dynContact.h>

// boost
#include <boost/bind.hpp>

// std
#include <numeric>

#include "Skin.hh"

using namespace yarp::math;

GZ_REGISTER_MODEL_PLUGIN(gazebo::GazeboYarpSkin)

namespace gazebo {

GazeboYarpSkin::GazeboYarpSkin() : m_rndGen(m_rndDev()) {}

GazeboYarpSkin::~GazeboYarpSkin()
{
    // Close the port
    m_portSkin.close();
    
    // Close the drivers
    m_drvTransformClient.close();
}

void GazeboYarpSkin::Load(gazebo::physics::ModelPtr _parent, sdf::ElementPtr _sdf)
{
    // Store pointer to the model
    m_model = _parent;


    // Store point to the SDF associated to the model
    m_sdf = _sdf;
    
    // Check yarp network availability
    if (!m_yarp.checkNetwork(GazeboYarpPlugins::yarpNetworkInitializationTimeout)) {
        yError() << "GazeboYarpSkin::Load error:"
		 << "yarp network does not seem to be available, is the yarpserver running?";
        return;
    }

    // Get .ini configuration file from plugin sdf
    bool ok = GazeboYarpPlugins::loadConfigModelPlugin(_parent, _sdf, m_parameters);
    if (!ok) {
        yError() << "GazeboYarpSkin::Load error:"
		 << "error loading .ini configuration from plugin SDF";
        return;
    }

    // Get hand type
    yarp::os::Value &whichHandValue = m_parameters.find("whichHand");
    if (whichHandValue.isNull()) {
        yError() << "GazeboYarpSkin::ConfigureAllContactSensors error:"
		 << "configuration parameter 'whichHand' not found.";
        return;
    }

    m_whichHand = whichHandValue.asString();

    // Get robot name from the SDF
    if (!LoadParam<std::string>("robotName", m_robotName))
	return;

    // Get source frame name required to retrieve the pose of the robot
    if (!LoadParam<std::string>("robotSourceFrameName", m_robotSourceFrameName))
	return;

    // Get target frame name required to retrieve the pose of the robot
    if (!LoadParam<std::string>("robotTargeFrameName", m_robotTargetFrameName))
	return;

    if (!LoadParam<std::string>("transformClientLocalPort", m_transformClientLocalPort))
	return;

    // Get the output port name
    if (!LoadParam<std::string>("outputPortName", m_outputPortName))
	return;

    // Get the noiseEnabled flag
    if (!LoadParam<bool>("enableNoise", m_noiseEnabled))
	return;

    // Configure gaussian random generator if required
    if (m_noiseEnabled) {
	// Load mean and standard deviation for gaussian noise
	double mean;
	double std;
	if (!LoadParam<double>("noiseMean", mean))
	    return;
	if (!LoadParam<double>("noiseStd", std))
	    return;

	// Configure the gaussian random rumber generator
	std::normal_distribution<>::param_type params(mean, std);
	m_gaussianGen.param(params);
    }

    // Prepare properties for the FrameTransformClient
    yarp::os::Property propTfClient;
    propTfClient.put("device", "transformClient");
    propTfClient.put("local", m_transformClientLocalPort);
    propTfClient.put("remote", "/transformServer");
	
    // try to open the driver
    ok = m_drvTransformClient.open(propTfClient);
    if (!ok) {
        yError() << "GazeboYarpSkin::Load error:"
		 << "unable to open the FrameTransformClient driver for the"
		 << m_whichHand
		 << "arm.";
	return;
    }
    
    // Try to retrieve the view
    ok = m_drvTransformClient.view(m_tfClient);
    if (!ok || m_tfClient == 0) {
        yError() << "GazeboYarpSkin::Load error:"
		 << "unable to retrieve the FrameTransformClient view for the"
		 << m_whichHand
		 << "arm.";
	return;
    }



    // Set default value
    m_robotRootFrameReceived = false;
    
    // Configure all the contact sensors
    ok = ConfigureAllContactSensors();
    if (!ok) {
	return;
    }

    // Open skin port
    ok = m_portSkin.open(m_outputPortName);//"/" + m_whichHand + "_hand/skinManager/skin_events:o"
    if (!ok)  {
        yError() << "GazeboYarpSkin::Load error:"
		 << "cannot open port /skinManager/skin_events:o";
        return;
    }
	
    // listen to the update event
    auto worldUpdateBind = boost::bind(&GazeboYarpSkin::OnWorldUpdate, this);
    m_worldUpdateConnection = gazebo::event::Events::ConnectWorldUpdateBegin(worldUpdateBind);
}

bool GazeboYarpSkin::RetrieveRobotRootFrame(ignition::math::Pose3d &pose)
{
    // Get the pose of the root frame of the robot
    // TODO: get source and target from configuration file
    yarp::sig::Matrix inertialToRobot(4,4);
    std::string source = m_robotSourceFrameName;//"/inertial";
    std::string target = m_robotTargetFrameName;//"/iCub/frame";

    // Get the transform 
    if (!m_tfClient->getTransform(target, source, inertialToRobot))
	return false;

    // Convert to ignition::math::Pose3d
    yarp::math::FrameTransform frame;
    frame.fromMatrix(inertialToRobot);

    yarp::math::FrameTransform::Translation_t  &pos = frame.translation;
    yarp::math::Quaternion &quat = frame.rotation;
    pose = ignition::math::Pose3d(pos.tX, pos.tY, pos.tZ,
				  quat.w(), quat.x(), quat.y(), quat.z());
    return true;
}

bool GazeboYarpSkin::RetrieveLinksFromLocalNames(const std::vector<std::string> &linksLocalNames,
						 linksMap &map)
{
    // Get all the links within the robot
    const gazebo::physics::Link_V &links = m_model->GetLinks();

    // Search for the given links
    // Lots of redundancy here, room for improvements...
    //
    for (size_t i=0; i<links.size(); i++)
    {
	// Get the scoped name of the current link
        std::string currentLinkScopedName = links[i]->GetScopedName();

	for (size_t j=0; j<linksLocalNames.size(); j++)
	{
	    // Check if the ending of the name of the current link corresponds to
	    // that of one of the given links
	    std::string linkNameScopedEnding = "::" + linksLocalNames[j];
	    if (GazeboYarpPlugins::hasEnding(currentLinkScopedName, linkNameScopedEnding))
	    {
                // Store the link into the map
                map[linksLocalNames[j]] = links[i];

                break;
            }
	}
    }

    // Return false if not all the links were found
    if (map.size() != linksLocalNames.size())
	return false;

    return true;
}

bool GazeboYarpSkin::ConfigureGazeboContactSensor(const std::string &linkLocalName,
						  ContactSensor &sensor)
{
    // Retrieve the link
    sensor.parentLink = m_linksMap[linkLocalName];
    if (sensor.parentLink == NULL)
        return false;

    // Check if this link contains any sensor
    size_t nSensors = sensor.parentLink->GetSensorCount();

    if (nSensors <= 0)
        return false;

    // One contact sensor per link is expected
    // however many sensors can be attached to the same link
    // hence it is required to search for the contact sensor
    sdf::ElementPtr modelSdf = sensor.parentLink->GetSDF();
    sdf::ElementPtr child;

    bool foundContactSensor = false;
    for (child = modelSdf->GetElement("sensor");
	 child != sdf::ElementPtr(nullptr);
	 child = child->GetNextElement("sensor"))
    {
	if ((child->GetAttribute("type")->GetAsString()) == "contact")
	{
	    // check if a child "contact" tag exists since it is not required
	    // by default
	    if (child->HasElement("contact"))
            {
                foundContactSensor = true;
                break;
	    }
	}
    }
    if (!foundContactSensor)
	return false;

    // Extract scoped sensor name
    std::string localSensorName = child->GetAttribute("name")->GetAsString();
    std::vector<std::string> scopedNameList = m_model->SensorScopedName(localSensorName);
    sensor.sensorName = std::accumulate(std::begin(scopedNameList),
                                        std::end(scopedNameList),
                                        sensor.sensorName);
    // Extract collision name
    sdf::ElementPtr contact = child->GetElement("contact");
    sensor.collisionName = contact->GetElement("collision")->GetValue()->GetAsString();

    // Get the sensor from the sensor manager
    gazebo::sensors::SensorManager *sensorMgr = gazebo::sensors::SensorManager::Instance();
    if (!sensorMgr->SensorsInitialized())
        return false;

    gazebo::sensors::SensorPtr genericPtr = sensorMgr->GetSensor(sensor.sensorName);
    sensor.sensor = std::dynamic_pointer_cast<gazebo::sensors::ContactSensor>(genericPtr);
    if (sensor.sensor == nullptr)
        return false;

    // Activate sensor
    sensor.sensor->SetActive(true);

    return true;

}

bool GazeboYarpSkin::ConfigureAllContactSensors()
{
    // Configure skinManager parameters
    iCub::skinDynLib::BodyPart bodyPart;
    iCub::skinDynLib::SkinPart skinPart;
    linkNumberEnum linkNumber = linkNumberEnum::HAND;

    if (m_whichHand == "right")
    {
	bodyPart = iCub::skinDynLib::BodyPart::RIGHT_ARM;
	skinPart = iCub::skinDynLib::SkinPart::SKIN_RIGHT_HAND;
    }
    else
    {
	bodyPart = iCub::skinDynLib::BodyPart::LEFT_ARM;
	skinPart = iCub::skinDynLib::SkinPart::SKIN_LEFT_HAND;
    }
    
    // Get local link names of the finger tips
    yarp::os::Bottle linksLocalNamesBottle = m_parameters.findGroup("linkNames");

    if (linksLocalNamesBottle.isNull()) {
        yError() << "GazeboYarpSkin::ConfigureAllContactSensors error:"
		 << "configuration parameter 'linkNames' not found.";
        return false;
    }

    int numberOfLinks = linksLocalNamesBottle.size()-1;
    for (size_t i=0; i<numberOfLinks; i++)
	linksLocalNames.push_back(linksLocalNamesBottle.get(i+1).asString().c_str());

    // Get taxel ids associated to each collision
    yarp::os::Bottle taxelIdsBottle = m_parameters.findGroup("taxelIds");
    std::vector<unsigned int> taxelIds;
    if (taxelIdsBottle.isNull()) {
        yError() << "GazeboYarpSkin::ConfigureAllContactSensors error:"
		 << "configuration parameter 'taxelIds' not found.";
        return false;
    }

    for (size_t i=0; i<numberOfLinks; i++)
        taxelIds.push_back(taxelIdsBottle.get((i+1)).asInt());

    // Retrieve the links from the model
    RetrieveLinksFromLocalNames(linksLocalNames, m_linksMap);

    // Configure contact sensors
    std::string linkLocalName;
    bool ok;
    for (size_t i=0; i<numberOfLinks; i++)
    {
	ContactSensor sensor;

	// Copy skinManager parameters
	sensor.bodyPart = bodyPart;
	sensor.linkNumber = linkNumber;
	sensor.skinPart = skinPart;
	sensor.taxelId = taxelIds[i];

	// Configure Gazebo contact sensor
    linkLocalName = linksLocalNames[i];
    // new
    std::cout << linkLocalName << std::endl;

	ok = ConfigureGazeboContactSensor(linkLocalName, sensor);
	if (!ok) {
	    yError() << "GazeboYarpSkin::ConfigureAllContactSensors error:"
		     << "cannot configure link"
		     << linkLocalName;
	    return false;
        }

	// Add sensor to the list
        m_contactSensors.push_back(sensor);
    }

    return true;
}

void GazeboYarpSkin::OnWorldUpdate()
{
    // set up parameters (exclude to the header file, that they are only set once!)
    double r = 0.007;                   // 7mm
    double xoff = 0.005;                // 5mm offset to the link frame
    double x1 = 0.0055+xoff;            // 5.5mm
    double x2 = 0.011+xoff;             // 11mm
    double x3 = r*cos(22.5*M_PI/180);   // in mm and deg -> express as rad
    double ymax = r*sin(18*M_PI/180);   // in mm and deg -> express as rad
    double y1 = r*sin(54*M_PI/180);     // in mm and deg -> express as rad
    double ymin = r;                    // in mm
    double ytop = r*sin(22.5*M_PI/180); // in mm and deg -> express as rad

    // The first time this is executed and
    // until m_robotRootFrameReceived is true
    // the transform between m_robotSourceFrameName
    // and m_robotTargetFrameName is retrieved
    if (!m_robotRootFrameReceived)
    {
	m_robotRootFrameReceived = RetrieveRobotRootFrame(m_inertialToRobot);
	if (!m_robotRootFrameReceived) {
	    yError() << "GazeboYarpSkin:OnWorldUpdate error:"
		     << "unable to get the pose of the root link of the robot.";
	    return;
	}
    }

    // Process contacts for each contact sensor
    iCub::skinDynLib::skinContactList &skinContactList = m_portSkin.prepare();
    skinContactList.clear();

    for (size_t i=0; i<m_contactSensors.size(); i++)
    {
	ContactSensor &sensor = m_contactSensors[i];
	msgs::Contacts contacts = sensor.sensor->Contacts();
	
	// Skip to next sensor if no contacts
	if (contacts.contact_size() == 0)
	    continue;

    // detect which hand is in contact
    std::string whichHand = m_whichHand;
    std::string HandName;
    std::cout << whichHand << std::endl;
    if (m_whichHand == "right")
    {
    HandName = "r_hand";
    }
    else
    {
    HandName = "l_hand";
    }

    // get pointer to the link
    gazebo::physics::LinkPtr link_name_ptr = m_model->GetLink("iCub::iCub::"+ HandName + "::" + linksLocalNames[i]);
    // std::cout << "linkName:" << std::endl;
    // std::cout << link_name_ptr << std::endl;

    // get link coordinates by using the pointer to the model
    ignition::math::Pose3d link_coord;
    link_coord = link_name_ptr->WorldPose();
    // std::cout << "linkCoordinates:" <<std::endl;
    // std::cout << link_coord << std::endl;

	size_t j =0;
	    for (size_t k=0; k<contacts.contact(j).position_size(); k++)
	    {
		
                // Extract position from message
                auto position = contacts.contact(j).position(k);

                // Convert to a pose with no rotation
                ignition::math::Pose3d point(position.x(), position.y(), position.z(),
                                             0, 0, 0);
                // std::cout << point << std::endl;

                // calculate the contact position at the fingertip
                ignition::math::Pose3d cont_tip = point - link_coord;
                std::cout << "contact in Link Coordinates Test:" <<std::endl;
                std::cout << cont_tip << std::endl;
                std::cout << cont_tip.Pos().X() << std::endl;

                int taxelId_tip;

                if (m_whichHand == "right")
                {
                    // calculate taxel IDs 1, 2 & 12:
                    if (xoff < cont_tip.Pos().X() < x1)
                    {
                        if (y1 < cont_tip.Pos().Y() < ymax)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                // no ID!
                                continue;
                            }
                            else
                            {
                                taxelId_tip = 1;
                            }
                        }
                        else if (ymin < cont_tip.Pos().Y() < y1)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                taxelId_tip = 12;
                            }
                            else
                            {
                                taxelId_tip = 2;
                            }
                        }
                    }
                    // calculate taxel IDs 3, 4, 10 & 11:
                    else if (x1 < cont_tip.Pos().X() < x2)
                    {
                        if (y1 < cont_tip.Pos().Y() < ymax)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                taxelId_tip = 11;
                            }
                            else
                            {
                                taxelId_tip = 3;
                            }
                        }
                        else if (ymin < cont_tip.Pos().Y() < y1)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                taxelId_tip = 10;
                            }
                            else
                            {
                                taxelId_tip = 4;
                            }
                        }
                    }
                    // calculate taxel IDs 5, 6, 8 & 9:
                    else if (x2 < cont_tip.Pos().X() < x3)
                    {
                        if (y1 < cont_tip.Pos().Y() < ymax)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                taxelId_tip = 9;
                            }
                            else
                            {
                                taxelId_tip = 5;
                            }
                        }
                        else if (ymin < cont_tip.Pos().Y() < y1)
                        {
                            if (0 < cont_tip.Pos().Z())
                            {
                                taxelId_tip = 8;
                            }
                            else
                            {
                                taxelId_tip = 6;
                            }
                    }
                    // calculate taxel ID 7:
                    else if (x3 < cont_tip.Pos().X() & -ytop < cont_tip.Pos().Y() < ytop )
                    {
                        taxelId_tip = 7;
                    }
                }
                else
                {
                    // figure out set up for left hand!!!
                }

		// Find the vector from the center of the root frame of the robot
		// to the contact points expressed in the root frame of the robot
		ignition::math::Pose3d diff = point - m_inertialToRobot;
		
		// Only geometric center of the contact is considered
		// in this implementation
		ignition::math::Vector3d & diffPos = diff.Pos();
		yarp::sig::Vector diffVector(3, 0.0);
                diffVector[0] = diffPos.X();
                diffVector[1] = diffPos.Y();
                diffVector[2] = diffPos.Z();

                // new
                //diffVector[0] = position.x();
                //diffVector[1] = position.y();
                //diffVector[2] = position.z();
		
		// normal
		yarp::sig::Vector normVector(3,0.0);
		normVector[0] = contacts.contact(j).normal(k).x();
		normVector[1] = contacts.contact(j).normal(k).y();
		normVector[2] = contacts.contact(j).normal(k).z();
		
		// force
		const gazebo::msgs::JointWrench& wrench = contacts.contact(j).wrench(k);
		yarp::sig::Vector forceVector(3,0.0);

		forceVector[0] = wrench.body_1_wrench().force().x();
		forceVector[1] = wrench.body_1_wrench().force().y();
		forceVector[2] = wrench.body_1_wrench().force().z();

		// Add noise if required
		if (m_noiseEnabled) {
		    diffVector[0] += m_gaussianGen(m_rndGen);
		    diffVector[1] += m_gaussianGen(m_rndGen);
		    diffVector[2] += m_gaussianGen(m_rndGen);
		}

		iCub::skinDynLib::dynContact dynContact(sensor.bodyPart,
							static_cast<int>(sensor.linkNumber),
							yarp::sig::Vector(3,0.0));
		iCub::skinDynLib::skinContact skinContact(dynContact);
		skinContact.setSkinPart(sensor.skinPart);
                skinContact.setGeoCenter(diffVector);
		skinContact.setNormalDir(normVector);
		skinContact.setForce(forceVector);

		// Suppose each contact is due to one taxel only
		skinContact.setActiveTaxels(1);

		// Set the right taxel id depending on the finger
		// involved in the contact
		std::vector<unsigned int> taxelIds;
        taxelIds.push_back(sensor.taxelId + taxelId_tip -1);
        skinContact.setTaxelList(taxelIds);

		// Add contact to the list
		skinContactList.push_back(skinContact);
	    }
	// }
    }
    
    // Send data over port in case of contacts
    if (skinContactList.size() != 0)
	m_portSkin.write();
}
    
}
}
