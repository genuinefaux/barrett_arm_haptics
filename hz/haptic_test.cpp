#include <stdio.h>
#include <cstdlib>  // For mkstmp()
#include <cstdio>  // For remove()

#include <boost/tuple/tuple.hpp>
#include <barrett/units.h>
#include <barrett/systems.h>
#include <barrett/products/product_manager.h>
#include <barrett/detail/stl_utils.h>

//#define BARRETT_SMF_VALIDATE_ARGS
#include <barrett/standard_main_function.h>
#include <barrett/log.h>



using namespace barrett;
using detail::waitForEnter;

// A System for getting force data from the force and torque sensor
template <size_t DOF>
class ftSystem : public systems::System {
    BARRETT_UNITS_TEMPLATE_TYPEDEFS(DOF);

public:		Input<double> input;
public:		Output<cf_type> output;
protected:	Output<cf_type>::Value* outputValue;

public:
	explicit ftSystem(ProductManager& pm,
			const std::string& sysName = "ftSystem") :
		    systems::System(sysName), input(this), output(this, &outputValue) {

            fts = NULL;
	        if (pm.foundForceTorqueSensor()) {
		        fts = pm.getForceTorqueSensor();
	        }

        }

    void tare()
    {
        if (fts != NULL)
        {
            fts->tare();
        }
    }

	virtual ~ftSystem() { mandatoryCleanUp(); }

protected:
	virtual void operate() {
        fts->update(true);
        outputValue->setData(&fts->getForce());  // Push data into the output
	}

    ForceTorqueSensor* fts;
};

/**
 * Controls the real-time mass damper spring simulation. This system simulates the
 * intended dynamics. Its output should be fed to a joint position control system
 * or an intermediary system such as the inverse kinematics solver.
 */
template <size_t DOF>
class MassDamperSim : public systems::SingleIO< boost::tuple<double, units::CartesianForce::type> , units::CartesianPosition::type> {
    BARRETT_UNITS_TEMPLATE_TYPEDEFS(DOF);

    public:
	    MassDamperSim(double M, double D, double K, double dist_lim, const std::string& sysName = "MassDamperSim") :
		    systems::SingleIO<boost::tuple<double, units::CartesianForce::type>, cp_type>(sysName), M(M), D(D), K(K),
            fy(0), q_ddot(0), q_dot(0), q(0), q_dot_p(0), q_p(-dist_lim), t_p(0), t_c(0), dT(0), dist_lim(dist_lim), min_force(0.9), spring_pos(0.0) {
            }

	    virtual ~MassDamperSim() { this->mandatoryCleanUp(); }

    protected:
        double M, D, K, fy;         // Mass, Damper, Spring, input force
        double q_ddot, q_dot, q;    // Spring acceleration, velocity, position
        double q_dot_p, q_p;        // Spring previous velocity, previous position
        double t_p, t_c, dT;        // time previous, time current, dT
        double dist_lim;            // limits the maximum value the spring can trvel to prevent singularities
        double min_force;           // Minimum force to prevent force sensor drift (clip when less than this)
        cp_type spring_pos;         // 3D position of the end of the spring

	    virtual void operate() {
            fy = boost::get<1>(this->input.getValue())[1];

            // Prevent arm from moving when no input
            fy = std::abs(fy)<min_force?0:fy;

            // Find the elapsed time
            t_c = boost::get<0>(this->input.getValue());
            dT = t_c - t_p;
           
            // Update the mass-damper simulation
            q_ddot = (fy - D*q_dot_p - K*q_p)/M;
            q_dot = q_dot_p + q_ddot*dT;
            q = q_p + q_dot*dT;
            t_p = t_c;

            // Update previous values for the next iteration
            if (std::abs(q) <= dist_lim || (fy>0 && q<0) || (fy<0 && q>0)) {
                q_dot_p = q_dot;
                q_p = q;
            } else {
                q_dot_p /= 2;
            }

            spring_pos[1] = q;//std::sin(t_c)/7;

		    this->outputValue->setData(&spring_pos);
	    }

    private:
	    DISALLOW_COPY_AND_ASSIGN(MassDamperSim);
};

// Controls the individual joints
template <size_t DOF>
class InverseK : public systems::SingleIO<units::CartesianPosition::type, typename units::JointPositions<DOF>::type> {
	BARRETT_UNITS_TEMPLATE_TYPEDEFS(DOF);

public:
	InverseK(const std::string& sysName = "InverseK") :
		systems::SingleIO<cp_type, jp_type>(sysName), jp_offset(0.0) {
		    i1 = 0;
			i2 = 3;

            double d_3 = 0.55;
            double d_T = 0.30;
            double a_3 = 0.045;

            x_offset = 0.68;    // Approximately the center of the range

            // Joint motor is offset the a zh-parameter
            l_1 = std::sqrt(std::pow(d_3,2)+std::pow(a_3,2));
            l_2 = std::sqrt(std::pow(d_T,2)+std::pow(a_3,2));

            // Joint angles must be offset as well
            jp_offset[i1] = -std::atan2(a_3, d_3);
            jp_offset[i2] = std::atan2(a_3, d_T);
		}

	virtual ~InverseK() { this->mandatoryCleanUp(); }

    // Moves the arm to the start position
    void gotoStartPosition(systems::Wam<DOF>& wam) {
        math::Matrix<3, 1, cp_type> origin;
        origin << 0,-0.2,0;

        compute_inverse_3D(origin);
        wam.moveTo(jp);
    }

protected:
	jp_type jp;
    jp_type jp_offset;
    double x_offset;    // Offset to serve as the origin or resting position
	double l_1, l_2;
	int i1, i2;

	virtual void operate() {
        compute_inverse_3D(this->input.getValue());
        this->outputValue->setData(&jp);
	}

    /**
     * Computes the joint angles resulting in the arm moving to the given
     * cartesian position.
     */
    void compute_inverse_3D(const math::Matrix<3, 1, cp_type> &dest)
    {
        // Compute the distance along the x-z line (the transformed x value)
        double trans_x = std::sqrt(std::pow(dest[0]+x_offset, 2)+std::pow(dest[2], 2))-x_offset;

        jp[1] = -M_PI_2-std::atan2(dest[2], dest[0]+x_offset);
        jp[2] = -M_PI_2;
        
        compute_inverse_2D(trans_x,dest[1]);
        jp[5] = -jp[i1]-jp[i2];
        jp[6] = -M_PI_2;
    }

    /**
     * Updates the joints labeled i1 and i2 in the jp array. The
     * joint angles are derived from the x,y coordinates given.
     */
    void compute_inverse_2D(double x, double y)
    {
        x += x_offset;

        // Compute theta1
        double l_squared = x*x + y*y;
        double gamma = std::acos((l_squared + l_1*l_1 - l_2*l_2)/(2*l_1*std::sqrt(l_squared)));
        jp[i1] = std::atan2(y,x) - gamma;

        // Compute theta2
        jp[i2] = std::atan2(y - l_1*std::sin(jp[i1]), x - l_1*cos(jp[i1]));
        jp[i2] -= jp[i1];

        // Align the final joint to the x-axis
        jp[i1] += jp_offset[i1];
        jp[i2] += jp_offset[i2];
    }

private:
	DISALLOW_COPY_AND_ASSIGN(InverseK);
};

/*bool validate_args(int argc, char** argv) {
	if (argc != 2) {
		printf("Usage: %s <fileName>\n", argv[0]);
		return false;
	}
	return true;
}*/

template<size_t DOF>
int wam_main(int argc, char** argv, ProductManager& pm, systems::Wam<DOF>& wam) {
	BARRETT_UNITS_TEMPLATE_TYPEDEFS(DOF);

    // Configure the force/torque sensor
    ftSystem<DOF> fts(pm);
    InverseK<DOF> jpc;
    systems::TupleGrouper<double, cf_type > mdsInput;
    MassDamperSim<DOF> mdsSim(10, 50, 0, 0.2);   // Spring Constants: M, D, K, spring distance
  
	wam.gravityCompensate();

	const double TRANSITION_DURATION = 0.5;  // seconds

	//Rate Limiter
	jp_type rt_jp_cmd;
	systems::RateLimiter<jp_type> jp_rl;

	//Sets the joints to move at 2 m/s
	const double rLimit[] = {2, 2, 2, 2, 2, 2, 2};

	for(size_t i = 0; i < DOF; ++i)
		rt_jp_cmd[i] = rLimit[i];

	systems::Ramp time(pm.getExecutionManager(), 1.0);

    if ( !pm.foundHand() ) {
		printf("ERROR: No Hand found on bus!\n");
		return 1;
	}
	Hand& hand = *pm.getHand();

    // Connect the systems
    systems::connect(time.output, fts.input);
    systems::connect(time.output, mdsInput.template getInput<0>());
    systems::connect(fts.output, mdsInput.template getInput<1>());
    systems::connect(mdsInput.output, mdsSim.input);
    systems::connect(mdsSim.output, jpc.input);

    char tmpFile[] = "/tmp/btXXXXXX";
	if (mkstemp(tmpFile) == -1) {
		printf("ERROR: Couldn't create temporary file!\n");
		return 1;
	}

    // Configure the logger
    systems::TupleGrouper<double, cf_type, cp_type> tg;
	connect(time.output, tg.template getInput<0>());
    connect(fts.output, tg.template getInput<1>());
    connect(mdsSim.output, tg.template getInput<2>());

    typedef boost::tuple<double, cf_type, cp_type> tuple_type;
	const size_t PERIOD_MULTIPLIER = 1;
	systems::PeriodicDataLogger<tuple_type> logger(
			pm.getExecutionManager(),
			new log::RealTimeWriter<tuple_type>(tmpFile, PERIOD_MULTIPLIER * pm.getExecutionManager()->getPeriod()),
			PERIOD_MULTIPLIER);

	printf("Press [Enter] to move to the start position.");
	waitForEnter();
    jpc.gotoStartPosition(wam);
    hand.initialize();

	//Indicate the current position and the maximum rate limit to the rate limiter
	jp_rl.setCurVal(wam.getJointPositions());
	jp_rl.setLimit(rt_jp_cmd);
	systems::connect(jpc.output, jp_rl.input);
    connect(tg.output, logger.input);
    
    int menu_option = 0;

    while (menu_option != 4) {
        printf( "\nMenu:\n"
                "1) Launch experiment\n"
                "2) Open Claw\n"
                "3) Close Claw\n"
                "4) Exit\n");

        if (scanf("%d", &menu_option))
        {
            getchar(); // Clean out buffer
            switch(menu_option)
            {
                case(1):
                {
                    printf("Logger state: %d\n", logger.isLogging());

	                wam.trackReferenceSignal(jp_rl.output);

	                time.smoothStart(TRANSITION_DURATION);

	                printf("Press [Enter] to stop.");
	                waitForEnter();
	                time.smoothStop(TRANSITION_DURATION);

                    // Save the logger info
                    logger.closeLog();
	                printf("Logging stopped.\n");

	                log::Reader<tuple_type> lr(tmpFile);

                    char filename[127];
                    printf("Enter a filename:");
	                if (scanf("%s", filename))
                    {
                        lr.exportCSV(filename);
	                    std::remove(tmpFile);
                        
                        FILE *fp; 
                        fp = fopen(tmpFile, "w");
                        fclose(fp);

                        getchar();
                    }
                    else
                    {
                        printf("Unable to save the file.\n");
                    }
                }
                    break;
                case(2):
                {
                    hand.open(Hand::GRASP);
                }
                    break;
                case(3):
                {
                    hand.close(Hand::GRASP);
                    printf("Press any key to tare the force sensor and continue.");
	                getchar();
                    fts.tare();
                }
                    break;
                case(4):
                {
                    printf("Exiting...\n");
                }
                    break;
                default:
                {                
                    printf("Unknown command. Please try again.\n");
                }
                    break;
            }

        }

    }

    // Shutdown Procedure
    hand.open(Hand::GRASP);
	wam.moveHome();
	wam.idle();

	// Wait for the user to press Shift-idle
	pm.getSafetyModule()->waitForMode(SafetyModule::IDLE);
	return 0;
}

