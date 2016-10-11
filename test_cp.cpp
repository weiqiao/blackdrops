#include <limbo/limbo.hpp>

#include <boost/numeric/odeint.hpp>
#include <boost/program_options.hpp>

#include <medrops/medrops.hpp>
#include <medrops/linear_policy.hpp>
#include <medrops/nn_policy.hpp>
#include <medrops/gp_model.hpp>
#include <medrops/exp_sq_ard.hpp>
#include <nn/nn.hpp>

#ifdef USE_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

//Screen dimension constants
const int SCREEN_WIDTH = 640;
const int SCREEN_HEIGHT = 480;

//The window we'll be rendering to
SDL_Window* window = NULL;

//The window renderer
SDL_Renderer* renderer = NULL;

bool sdl_init()
{
    //Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cout << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }

    window = SDL_CreateWindow("Cartpole Task", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (window == NULL) {
        std::cout << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }

    //Create renderer for window
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        std::cout << "Renderer could not be created! SDL Error: " << SDL_GetError() << std::endl;
        return false;
    }

    //Initialize renderer color
    SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);

    //Update the surface
    SDL_UpdateWindowSurface(window);

    //If everything initialized fine
    return true;
}

bool draw_cartpole(double x, double theta, bool red = false)
{
    double th_x = std::cos(theta), th_y = std::sin(theta);

    SDL_Rect outlineRect = {SCREEN_WIDTH / 2 - x * SCREEN_HEIGHT / 4 - 0.1 * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 2 - 0.05 * SCREEN_HEIGHT / 4, 0.2 * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
    SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0xFF, 0xFF);
    if (red)
        SDL_SetRenderDrawColor(renderer, 0xFF, 0x00, 0x00, 0xFF);
    SDL_RenderFillRect(renderer, &outlineRect);

    SDL_RenderDrawLine(renderer, SCREEN_WIDTH / 2 - x * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 2, SCREEN_WIDTH / 2 - x * SCREEN_HEIGHT / 4 + th_y * SCREEN_HEIGHT / 8, SCREEN_HEIGHT / 2 + th_x * SCREEN_HEIGHT / 8);

    return true;
}

bool draw_goal(double x, double y)
{
    SDL_Rect outlineRect = {SCREEN_WIDTH / 2 - 0.05 * SCREEN_HEIGHT / 4 + x * SCREEN_HEIGHT / 4, (1 - y) * SCREEN_HEIGHT / 4 - 0.05 * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
    SDL_SetRenderDrawColor(renderer, 0xFF, 0x00, 0x00, 0xFF);
    SDL_RenderFillRect(renderer, &outlineRect);

    return true;
}

void sdl_clean()
{
    // Destroy renderer
    SDL_DestroyRenderer(renderer);
    //Destroy window
    SDL_DestroyWindow(window);
    //Quit SDL
    SDL_Quit();
}
#endif

template <typename T>
inline T gaussian_rand(T m = 0.0, T v = 1.0)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    std::normal_distribution<T> gaussian(m, v);

    return gaussian(gen);
}

struct Params {
    BO_PARAM(size_t, action_dim, 1);
    BO_PARAM(size_t, state_full_dim, 6);
    BO_PARAM(size_t, model_input_dim, 5);
    BO_PARAM(size_t, model_pred_dim, 4);

    BO_DYN_PARAM(size_t, parallel_evaluations);

    BO_PARAM(double, goal_pos, M_PI);
    BO_PARAM(double, goal_vel, 0.0);
    BO_PARAM(double, goal_pos_x, 0.0);
    BO_PARAM(double, goal_vel_x, 0.0);

    struct medrops {
        BO_PARAM(size_t, rollout_steps, 40);
    };

    struct gp_model {
        BO_PARAM(double, noise, 0.01);
    };

    struct linear_policy {
        BO_PARAM(int, state_dim, 5);
        BO_PARAM(double, max_u, 10.0);
    };

    struct nn_policy {
        BO_PARAM(int, state_dim, 5);
        BO_PARAM(double, max_u, 10.0);
        BO_DYN_PARAM(int, hidden_neurons);
    };

    struct mean_constant {
        BO_PARAM(double, constant, 0.0);
    };

    struct opt_nloptgrad : public limbo::defaults::opt_nloptgrad {
        BO_PARAM(int, iterations, 1000);
    };

    struct opt_cmaes : public limbo::defaults::opt_cmaes {
        BO_DYN_PARAM(double, max_fun_evals);
    };
};

struct GPParams {
    struct opt_cmaes : public limbo::defaults::opt_cmaes {
    };
};

inline double angle_dist(double a, double b)
{
    double theta = b - a;
    while (theta < -M_PI)
        theta += 2 * M_PI;
    while (theta > M_PI)
        theta -= 2 * M_PI;
    return theta;
}

namespace global {
    std::vector<Eigen::VectorXd> _tried_policies = std::vector<Eigen::VectorXd>();
    std::vector<Eigen::VectorXd> _tried_rewards = std::vector<Eigen::VectorXd>();
}

struct CartPole {
    typedef std::vector<double> ode_state_type;

    template <typename Policy, typename Reward>
    std::vector<std::tuple<Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd>> execute(const Policy& policy, const Reward& world, size_t steps, std::vector<double>& R)
    {
        double dt = 0.1;
        std::vector<std::tuple<Eigen::VectorXd, Eigen::VectorXd, Eigen::VectorXd>> res;

        boost::numeric::odeint::runge_kutta4<ode_state_type> ode_stepper;
        double t = 0.0;
        R = std::vector<double>();

        ode_state_type cp_state(4, 0.0);
        // Policy policy;
        // Eigen::VectorXd params(6);
        // params << 0.6717, 0.2685, 0.0066, 0.6987, 0.4845, 3.1517;
        // policy.set_params(params);

        for (size_t i = 0; i < steps; i++) {
            Eigen::VectorXd init(Params::model_input_dim());
            init(0) = cp_state[0];
            init(1) = cp_state[1];
            init(2) = cp_state[2];
            init(3) = std::cos(cp_state[3]);
            init(4) = std::sin(cp_state[3]);

            Eigen::VectorXd init_diff = Eigen::VectorXd::Map(cp_state.data(), cp_state.size());
            // while (init_diff(1) < -M_PI)
            //     init_diff(1) += 2 * M_PI;
            // while (init_diff(1) > M_PI)
            //     init_diff(1) -= 2 * M_PI;

            _u = policy.next(init)[0];
            ode_stepper.do_step(std::bind(&CartPole::dynamics, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3), cp_state, t, dt);
            t += dt;
            // if (cp_state[0] < -2)
            //     cp_state[0] = -2;
            // if (cp_state[0] > 2)
            //     cp_state[0] = 2;
            Eigen::VectorXd final = Eigen::VectorXd::Map(cp_state.data(), cp_state.size());
            // while (final(1) < -M_PI)
            //     final(1) += 2 * M_PI;
            // while (final(1) > M_PI)
            //     final(1) -= 2 * M_PI;
            res.push_back(std::make_tuple(init, limbo::tools::make_vector(_u), final - init_diff));
            double r = world(init, limbo::tools::make_vector(_u), final);
            R.push_back(r);
#ifdef USE_SDL
            //Clear screen
            SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);
            SDL_RenderClear(renderer);

            draw_cartpole(cp_state[0], cp_state[3]);
            draw_goal(0, -0.5);

            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF);
            SDL_Rect outlineRect = {SCREEN_WIDTH / 2 + 0.05 * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 4 + 2.05 * SCREEN_HEIGHT / 4, _u / 10.0 * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
            SDL_RenderFillRect(renderer, &outlineRect);

            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0xFF, 0xFF);
            outlineRect = {SCREEN_WIDTH / 2 + 0.05 * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 4 + 2.55 * SCREEN_HEIGHT / 4, r * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
            SDL_RenderFillRect(renderer, &outlineRect);

            //Update screen
            SDL_RenderPresent(renderer);

            SDL_Delay(dt * 1000);
#endif
        }

        if (!policy.random()) {
            global::_tried_policies.push_back(policy.params());
            double rr = std::accumulate(R.begin(), R.end(), 0.0);
            std::cout << "Reward: " << rr << std::endl;
            global::_tried_rewards.push_back(limbo::tools::make_vector(rr));
        }

        return res;
    }

    template <typename Policy, typename Model, typename Reward>
    void execute_dummy(const Policy& policy, const Model& model, const Reward& world, size_t steps, std::vector<double>& R) const
    {
        R = std::vector<double>();
        // init state
        Eigen::VectorXd init_diff = Eigen::VectorXd::Zero(Params::model_pred_dim());
        Eigen::VectorXd init = Eigen::VectorXd::Zero(Params::model_input_dim());
        init(3) = std::cos(0.0);
        init(4) = std::sin(0.0);
        for (size_t j = 0; j < steps; j++) {
            Eigen::VectorXd query_vec(Params::model_input_dim() + Params::action_dim());
            Eigen::VectorXd u = policy.next(init);
            query_vec.head(Params::model_input_dim()) = init;
            query_vec.tail(Params::action_dim()) = u;

            Eigen::VectorXd mu;
            double sigma;
            std::tie(mu, sigma) = model.predict(query_vec);
            sigma = std::sqrt(sigma);
            for (int i = 0; i < mu.size(); i++) {
                double s = gaussian_rand(mu(i), sigma);
                mu(i) = std::max(mu(i) - sigma, std::min(s, mu(i) + sigma));
            }

            Eigen::VectorXd final = init_diff + mu;
            // if (final(0) < -2)
            //     final(0) = -2;
            // if (final(0) > 2)
            //     final(0) = 2;
            // while (final(1) < -M_PI)
            //     final(1) += 2 * M_PI;
            // while (final(1) > M_PI)
            //     final(1) -= 2 * M_PI;
            double r = world(init_diff, mu, final);
            R.push_back(r);
            // reward += world(init_diff, u, final);
            init_diff = final;
            init(0) = final(0);
            init(1) = final(1);
            init(2) = final(2);
            init(3) = std::cos(final(3));
            init(4) = std::sin(final(3));

#ifdef USE_SDL
            double dt = 0.1;
            //Clear screen
            SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);
            SDL_RenderClear(renderer);

            draw_cartpole(init_diff[0], init_diff[3], true);
            draw_goal(0, -0.5);

            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF);
            SDL_Rect outlineRect = {SCREEN_WIDTH / 2 + 0.05 * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 4 + 2.05 * SCREEN_HEIGHT / 4, u[0] / 10.0 * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
            SDL_RenderFillRect(renderer, &outlineRect);

            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0xFF, 0xFF);
            outlineRect = {SCREEN_WIDTH / 2 + 0.05 * SCREEN_HEIGHT / 4, SCREEN_HEIGHT / 4 + 2.55 * SCREEN_HEIGHT / 4, r * SCREEN_HEIGHT / 4, 0.1 * SCREEN_HEIGHT / 4};
            SDL_RenderFillRect(renderer, &outlineRect);

            //Update screen
            SDL_RenderPresent(renderer);

            SDL_Delay(dt * 1000);
#endif
        }
    }

    template <typename Policy, typename Model, typename Reward>
    double predict_policy(const Policy& policy, const Model& model, const Reward& world, size_t steps) const
    {
        size_t N = Params::parallel_evaluations();

        double* rews = new double[N];

        tbb::parallel_for(size_t(0), N, size_t(1), [&](size_t i) {
          double reward = 0.0;
          // init state
          Eigen::VectorXd init_diff = Eigen::VectorXd::Zero(Params::model_pred_dim());
          Eigen::VectorXd init = Eigen::VectorXd::Zero(Params::model_input_dim());
          init(3) = std::cos(0.0);
          init(4) = std::sin(0.0);
          for (size_t j = 0; j < steps; j++) {
              Eigen::VectorXd query_vec(Params::model_input_dim() + Params::action_dim());
              Eigen::VectorXd u = policy.next(init);
              query_vec.head(Params::model_input_dim()) = init;
              query_vec.tail(Params::action_dim()) = u;

              Eigen::VectorXd mu;
              double sigma;
              std::tie(mu, sigma) = model.predict(query_vec);
              sigma = std::sqrt(sigma);
              for (int i = 0; i < mu.size(); i++) {
                  double s = gaussian_rand(mu(i), sigma);
                  mu(i) = std::max(mu(i) - sigma, std::min(s, mu(i) + sigma));
              }

              Eigen::VectorXd final = init_diff + mu;
              // if(final(0) < -2)
              //     final(0) = -2;
              // if(final(0) > 2)
              //     final(0) = 2;
              // while (final(1) < -M_PI)
              //     final(1) += 2 * M_PI;
              // while (final(1) > M_PI)
              //     final(1) -= 2 * M_PI;
              reward += world(init_diff, u, final);
              init_diff = final;
              init(0) = final(0);
              init(1) = final(1);
              init(2) = final(2);
              init(3) = std::cos(final(3));
              init(4) = std::sin(final(3));
          }
          rews[i] = reward;
        });

        double r = 0.0;
        for (size_t i = 0; i < N; i++)
            r += rews[i];
        r /= double(N);

        delete[] rews;

        return r;
    }

    /* The rhs of x' = f(x) */
    void dynamics(const ode_state_type& x, ode_state_type& dx, double t)
    {
        double l = 0.5, m = 0.5, M = 0.5, g = 9.82, b = 0.1;

        dx[0] = x[1];
        dx[1] = (2 * m * l * std::pow(x[2], 2.0) * std::sin(x[3]) + 3 * m * g * std::sin(x[3]) * std::cos(x[3]) + 4 * _u - 4 * b * x[1]) / (4 * (M + m) - 3 * m * std::pow(std::cos(x[3]), 2.0));
        dx[2] = (-3 * m * l * std::pow(x[2], 2.0) * std::sin(x[3]) * std::cos(x[3]) - 6 * (M + m) * g * std::sin(x[3]) - 6 * (_u - b * x[1]) * std::cos(x[3])) / (4 * l * (m + M) - 3 * m * l * std::pow(std::cos(x[3]), 2.0));
        dx[3] = x[2];
        // dx[0] = x[1];
        // dx[1] = (_u + m * std::sin(x[3]) * (l * x[2] * x[2] + g * std::cos(x[3]))) / (M + m * std::cos(x[3]) * std::cos(x[3]));
        // dx[2] = (-_u * std::cos(x[3]) - m * l * x[2] * x[2] * std::cos(x[3]) * std::sin(x[3]) - (M + m) * g * std::sin(x[3])) / (l * (M + m * std::sin(x[3]) * std::sin(x[3])));
        // dx[3] = x[2];
    }

protected:
    double _u;
};

struct RewardFunction {
    double operator()(const Eigen::VectorXd& from_state, const Eigen::VectorXd& action, const Eigen::VectorXd& to_state) const
    {
        double s_c_sq = 0.25 * 0.25;
        double dx = angle_dist(to_state(3), Params::goal_pos());
        double dy = to_state(2) - Params::goal_vel();
        double dz = to_state(1) - Params::goal_vel_x();
        double dw = to_state(0) - Params::goal_pos_x();

        return std::exp(-0.5 / s_c_sq * (dx * dx + dy * dy + dz * dz + dw * dw));
    }
};

using kernel_t = medrops::SquaredExpARD<Params>;
// using kernel_t = limbo::kernel::Exp<Params>;
using mean_t = limbo::mean::Constant<Params>;
using GP_t = limbo::model::GP<Params, kernel_t, mean_t, limbo::model::gp::KernelLFOpt<Params, limbo::opt::Cmaes<GPParams>>>;

BO_DECLARE_DYN_PARAM(size_t, Params, parallel_evaluations);
BO_DECLARE_DYN_PARAM(int, Params::nn_policy, hidden_neurons);
BO_DECLARE_DYN_PARAM(double, Params::opt_cmaes, max_fun_evals);

int main(int argc, char** argv)
{
    namespace po = boost::program_options;
    po::options_description desc("Command line arguments");
    desc.add_options()("help,h", "Prints this help message")("parallel_evaluations,p", po::value<int>(), "Number of parallel monte carlo evaluations for policy reward estimation.")("hidden_neurons,n", po::value<int>(), "Number of hidden neurons in NN policy.")("max_evals,m", po::value<int>(), "Max function evaluations to optimize the policy.");

    try {
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }

        po::notify(vm);

        if (vm.count("parallel_evaluations")) {
            int c = vm["parallel_evaluations"].as<int>();
            if (c < 0)
                c = 0;
            Params::set_parallel_evaluations(c);
        }
        else {
            Params::set_parallel_evaluations(100);
        }
        if (vm.count("hidden_neurons")) {
            int c = vm["hidden_neurons"].as<int>();
            if (c < 1)
                c = 1;
            Params::nn_policy::set_hidden_neurons(c);
        }
        else {
            Params::nn_policy::set_hidden_neurons(5);
        }
        if (vm.count("max_evals")) {
            int c = vm["max_evals"].as<int>();
            Params::opt_cmaes::set_max_fun_evals(c);
        }
        else {
            Params::opt_cmaes::set_max_fun_evals(10000);
        }
    }
    catch (po::error& e) {
        std::cerr << "[Exception caught while parsing command line arguments]: " << e.what() << std::endl;
        return 1;
    }
#ifdef USE_SDL
    //Initialize
    if (!sdl_init()) {
        return 1;
    }
#endif

    medrops::Medrops<Params, medrops::GPModel<Params, GP_t>, CartPole, medrops::NNPolicy<Params>, limbo::opt::Cmaes<Params>, RewardFunction> cp_system;

    cp_system.learn(1, 10);

#ifdef USE_SDL
    sdl_clean();
#endif

    return 0;
}